// pybind11 bindings for the order book engine (lob::core) and the Avellaneda-Stoikov replication
// (lob::paper). Built as the extension module lobsim._lobsim; the public package is python/lobsim/.
//
// Conventions carried over from C++ (see core/include/lob/order_book.hpp):
//   - Mutating book calls report errors as Status values and never raise for bad values; a
//     rejected call leaves the book unchanged. Arguments of the wrong Python type raise TypeError
//     (pybind11's normal overload resolution).
//   - Calls that can trade return a SubmitResult that also carries the fills of that call.
// Python-only additions:
//   - replay_lobster() and paper.simulate() release the GIL. While replay_lobster() runs on a book,
//     any other call on that book (from another thread) raises RuntimeError instead of racing.
#include <pybind11/native_enum.h>
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "lob/avellaneda_stoikov.hpp"
#include "lob/order_book.hpp"
#include "lob/paper_experiment.hpp"
#include "lob/types.hpp"

namespace py = pybind11;
using namespace py::literals;

namespace {

using lob::Fill;
using lob::Level;
using lob::OrderBook;
using lob::OrderId;
using lob::OrderInfo;
using lob::Price;
using lob::Qty;
using lob::Side;
using lob::Status;
using lob::Time;

// ---- Value classes -----------------------------------------------------------------------------

// Result of a call that can trade, together with the fills that call produced.
struct PySubmitResult {
  lob::SubmitResult r;
  std::vector<Fill> fills;
};

std::string side_name(Side s) { return s == Side::Bid ? "Side.Bid" : "Side.Ask"; }

std::string fill_repr(const Fill& f) {
  return "Fill(resting_id=" + std::to_string(f.resting_id) +
         ", aggressor_id=" + std::to_string(f.aggressor_id) +
         ", resting_side=" + side_name(f.resting_side) + ", price=" + std::to_string(f.price) +
         ", qty=" + std::to_string(f.qty) + ", ts=" + std::to_string(f.ts) +
         ", resting_left=" + std::to_string(f.resting_left) + ")";
}

bool fill_eq(const Fill& a, const Fill& b) {
  return a.resting_id == b.resting_id && a.aggressor_id == b.aggressor_id &&
         a.resting_side == b.resting_side && a.price == b.price && a.qty == b.qty &&
         a.ts == b.ts && a.resting_left == b.resting_left;
}

py::tuple fills_tuple(const std::vector<Fill>& fills) {
  py::tuple t(fills.size());
  for (std::size_t i = 0; i < fills.size(); ++i) t[i] = py::cast(fills[i]);
  return t;
}

// ---- Book wrapper ------------------------------------------------------------------------------

class PyBook {
 public:
  // Every entry point calls this. busy_ is only written while the GIL is held, so the GIL orders
  // these reads and writes; no atomic is needed.
  void guard() const {
    if (busy_) {
      throw std::runtime_error(
          "OrderBook is in use by replay_lobster() in another thread; the book is not "
          "thread-safe");
    }
  }

  OrderBook& book() {
    guard();
    return book_;
  }
  const OrderBook& cbook() const {
    guard();
    return book_;
  }

  PySubmitResult submit_limit(OrderId id, Side side, Price price, Qty qty, Time ts) {
    PySubmitResult out;
    out.r = book().submit_limit(id, side, price, qty, ts, out.fills);
    return out;
  }

  PySubmitResult submit_market(OrderId id, Side side, Qty qty, Time ts) {
    PySubmitResult out;
    out.r = book().submit_market(id, side, qty, ts, out.fills);
    return out;
  }

  PySubmitResult replace(OrderId id, Price new_price, Qty new_qty, Time ts) {
    PySubmitResult out;
    out.r = book().replace(id, new_price, new_qty, ts, out.fills);
    return out;
  }

  // C++ returns only a Status; in Python the fill comes back in a SubmitResult with
  // filled = executed size and resting = size left under this id.
  PySubmitResult execute_resting(OrderId id, Qty qty, Time ts) {
    PySubmitResult out;
    out.r.status = book().execute_resting(id, qty, ts, out.fills);
    if (out.r.status == Status::Ok && !out.fills.empty()) {
      out.r.filled = out.fills.back().qty;
      out.r.resting = out.fills.back().resting_left;
    }
    return out;
  }

  py::array_t<std::int64_t> depth(Side side, py::ssize_t n) {
    if (n < 0) throw py::value_error("depth: n must be >= 0, got " + std::to_string(n));
    cbook().depth(side, static_cast<std::size_t>(n), levels_);
    const auto k = static_cast<py::ssize_t>(levels_.size());
    py::array_t<std::int64_t> out({k, static_cast<py::ssize_t>(3)});
    auto w = out.mutable_unchecked<2>();
    for (py::ssize_t i = 0; i < k; ++i) {
      const Level& lv = levels_[static_cast<std::size_t>(i)];
      w(i, 0) = lv.price;
      w(i, 1) = lv.qty;
      w(i, 2) = static_cast<std::int64_t>(lv.count);
    }
    return out;
  }

  py::tuple check_invariants(bool require_uncrossed) const {
    std::string why;
    const bool ok = cbook().check_invariants(require_uncrossed, &why);
    return py::make_tuple(ok, why);
  }

  std::string repr() const {
    const OrderBook& b = cbook();
    auto top = [](const std::optional<Level>& lv) {
      return lv ? std::to_string(lv->qty) + "@" + std::to_string(lv->price) : std::string("None");
    };
    return "OrderBook(orders=" + std::to_string(b.order_count()) +
           ", best_bid=" + top(b.best_bid()) + ", best_ask=" + top(b.best_ask()) + ")";
  }

  // For replay_lobster: the raw book, marked busy for the duration (see ReplayBusy).
  OrderBook& raw() { return book_; }
  bool busy_ = false;

 private:
  OrderBook book_;
  std::vector<Level> levels_;  // scratch for depth()
};

// Sets the busy flag for the lifetime of the object. Construct and destroy with the GIL held.
class ReplayBusy {
 public:
  explicit ReplayBusy(PyBook& b) : b_(b) {
    b_.guard();
    b_.busy_ = true;
  }
  ~ReplayBusy() { b_.busy_ = false; }
  ReplayBusy(const ReplayBusy&) = delete;
  ReplayBusy& operator=(const ReplayBusy&) = delete;

 private:
  PyBook& b_;
};

// ---- Bulk replay -------------------------------------------------------------------------------

constexpr int kTypeSlots = 8;  // index 1..7 = LOBSTER types, index 0 = any other type value

struct ReplayResult {
  std::int64_t messages = 0;
  std::int64_t rejected = 0;
  std::int64_t first_rejected = -1;
  py::array_t<std::int64_t> count_by_type;
  py::array_t<std::int64_t> rejected_by_type;
  py::object bid_price = py::none();
  py::object bid_qty = py::none();
  py::object ask_price = py::none();
  py::object ask_qty = py::none();
};

template <class T>
py::detail::unchecked_reference<T, 1> checked_column(const py::object& obj, const char* name,
                                                     const char* dtype_name,
                                                     py::ssize_t expected_len) {
  if (!py::isinstance<py::array>(obj)) {
    throw py::type_error(std::string("replay_lobster: '") + name +
                         "' must be a numpy.ndarray, got " +
                         std::string(py::str(py::type::of(obj).attr("__name__"))));
  }
  const auto arr = py::reinterpret_borrow<py::array>(obj);
  if (!py::isinstance<py::array_t<T>>(arr)) {
    throw py::value_error(std::string("replay_lobster: '") + name + "' must have dtype " +
                          dtype_name + " (native byte order), got " +
                          std::string(py::str(arr.dtype())) + "; convert it with .astype(np." +
                          dtype_name + ")");
  }
  if (arr.ndim() != 1) {
    throw py::value_error(std::string("replay_lobster: '") + name +
                          "' must be one-dimensional, got ndim=" + std::to_string(arr.ndim()));
  }
  if (expected_len >= 0 && arr.shape(0) != expected_len) {
    throw py::value_error(std::string("replay_lobster: all arrays must have the same length; '") +
                          name + "' has length " + std::to_string(arr.shape(0)) +
                          " but 'time_ns' has length " + std::to_string(expected_len));
  }
  return py::reinterpret_borrow<py::array_t<T>>(arr).template unchecked<1>();
}

ReplayResult replay_lobster(PyBook& pybook, const py::object& time_ns, const py::object& type,
                            const py::object& order_id, const py::object& size,
                            const py::object& price, const py::object& direction,
                            bool capture_top) {
  const auto t = checked_column<std::int64_t>(time_ns, "time_ns", "int64", -1);
  const py::ssize_t n = t.shape(0);
  const auto ty = checked_column<std::int8_t>(type, "type", "int8", n);
  const auto id = checked_column<std::uint64_t>(order_id, "order_id", "uint64", n);
  const auto sz = checked_column<std::int64_t>(size, "size", "int64", n);
  const auto px = checked_column<std::int64_t>(price, "price", "int64", n);
  const auto dir = checked_column<std::int8_t>(direction, "direction", "int8", n);

  ReplayResult res;
  res.messages = n;
  res.count_by_type = py::array_t<std::int64_t>(kTypeSlots);
  res.rejected_by_type = py::array_t<std::int64_t>(kTypeSlots);
  std::int64_t* counts = res.count_by_type.mutable_data();
  std::int64_t* rejects = res.rejected_by_type.mutable_data();
  for (int i = 0; i < kTypeSlots; ++i) counts[i] = rejects[i] = 0;

  std::int64_t* bp = nullptr;
  std::int64_t* bq = nullptr;
  std::int64_t* ap = nullptr;
  std::int64_t* aq = nullptr;
  if (capture_top) {
    py::array_t<std::int64_t> a(n), b(n), c(n), d(n);
    bp = a.mutable_data();
    bq = b.mutable_data();
    ap = c.mutable_data();
    aq = d.mutable_data();
    res.bid_price = std::move(a);
    res.bid_qty = std::move(b);
    res.ask_price = std::move(c);
    res.ask_qty = std::move(d);
  }

  ReplayBusy busy(pybook);
  OrderBook& book = pybook.raw();
  std::int64_t rejected = 0;
  std::int64_t first_rejected = -1;
  {
    py::gil_scoped_release release;
    std::vector<Fill> fills;
    fills.reserve(1);
    for (py::ssize_t i = 0; i < n; ++i) {
      const int mtype = ty(i);
      const int slot = (mtype >= 1 && mtype <= 7) ? mtype : 0;
      ++counts[slot];
      bool ok = true;
      switch (mtype) {
        case 1: {
          const int d = dir(i);
          if (d == 1 || d == -1) {
            ok = book.add_resting(id(i), static_cast<Side>(d), px(i), sz(i), t(i)) == Status::Ok;
          } else {
            ok = false;  // not a side
          }
          break;
        }
        case 2: ok = book.reduce(id(i), sz(i)) == Status::Ok; break;
        case 3: ok = book.cancel(id(i)) == Status::Ok; break;
        case 4:
          fills.clear();
          ok = book.execute_resting(id(i), sz(i), t(i), fills) == Status::Ok;
          break;
        case 5:
        case 6:
        case 7: break;  // hidden execution, cross trade, halt: visible book unchanged
        default: ok = false; break;
      }
      if (!ok) {
        ++rejects[slot];
        if (rejected++ == 0) first_rejected = i;
      }
      if (capture_top) {
        const auto lv_b = book.best_bid();
        const auto lv_a = book.best_ask();
        const auto k = static_cast<std::size_t>(i);
        bp[k] = lv_b ? lv_b->price : 0;
        bq[k] = lv_b ? lv_b->qty : 0;
        ap[k] = lv_a ? lv_a->price : 0;
        aq[k] = lv_a ? lv_a->qty : 0;
      }
    }
  }
  res.rejected = rejected;
  res.first_rejected = first_rejected;
  return res;
}

// ---- Paper replication -------------------------------------------------------------------------

struct SimResults {
  std::vector<lob::paper::SimResult> sims;
  py::array_t<double> profit, cash, final_mid, mean_spread;
  py::array_t<std::int64_t> final_q, bid_fills, ask_fills;
  double gamma = 0.0;
  std::string strategy;
  std::uint64_t seed = 0;
};

lob::paper::ModelParams make_model(double s0, double sigma, double horizon, double A, double k,
                                   int steps) {
  auto finite = [](double x) { return std::isfinite(x); };
  if (!finite(s0)) throw py::value_error("s0 must be finite");
  if (!finite(sigma) || sigma < 0) throw py::value_error("sigma must be finite and >= 0");
  if (!finite(horizon) || horizon <= 0) throw py::value_error("horizon must be finite and > 0");
  if (!finite(A) || A < 0) throw py::value_error("A must be finite and >= 0");
  if (!finite(k) || k <= 0) throw py::value_error("k must be finite and > 0");
  if (steps < 1) throw py::value_error("steps must be >= 1");
  lob::paper::ModelParams p;
  p.s0 = s0;
  p.sigma = sigma;
  p.horizon = horizon;
  p.A = A;
  p.k = k;
  p.steps = steps;
  return p;
}

SimResults simulate(double gamma, int sims, std::uint64_t seed, const std::string& strategy,
                    double s0, double sigma, double horizon, double A, double k, int steps) {
  if (!std::isfinite(gamma) || gamma <= 0) throw py::value_error("gamma must be finite and > 0");
  if (sims < 0) throw py::value_error("sims must be >= 0");
  lob::paper::Strategy strat{};
  if (!lob::paper::parse_strategy(strategy.c_str(), strat)) {
    throw py::value_error("unknown strategy '" + strategy +
                          "'; expected 'inventory', 'symmetric' or 'symmetric_tv'");
  }
  lob::paper::ExperimentConfig cfg;
  cfg.model = make_model(s0, sigma, horizon, A, k, steps);
  cfg.gammas = {gamma};
  cfg.strategies = {strat};
  cfg.seed = seed;
  cfg.sims = sims;

  SimResults out;
  {
    py::gil_scoped_release release;
    lob::paper::ExperimentResults r = lob::paper::run_experiment(cfg);
    out.sims = std::move(r[0][0]);
  }
  const auto n = static_cast<py::ssize_t>(out.sims.size());
  out.profit = py::array_t<double>(n);
  out.cash = py::array_t<double>(n);
  out.final_mid = py::array_t<double>(n);
  out.mean_spread = py::array_t<double>(n);
  out.final_q = py::array_t<std::int64_t>(n);
  out.bid_fills = py::array_t<std::int64_t>(n);
  out.ask_fills = py::array_t<std::int64_t>(n);
  double* profit = out.profit.mutable_data();
  double* cash = out.cash.mutable_data();
  double* final_mid = out.final_mid.mutable_data();
  double* mean_spread = out.mean_spread.mutable_data();
  std::int64_t* final_q = out.final_q.mutable_data();
  std::int64_t* bid_fills = out.bid_fills.mutable_data();
  std::int64_t* ask_fills = out.ask_fills.mutable_data();
  for (std::size_t i = 0; i < out.sims.size(); ++i) {
    const lob::paper::SimResult& s = out.sims[i];
    profit[i] = s.profit;
    cash[i] = s.cash;
    final_mid[i] = s.final_mid;
    mean_spread[i] = s.mean_spread;
    final_q[i] = s.final_q;
    bid_fills[i] = s.bid_fills;
    ask_fills[i] = s.ask_fills;
  }
  out.gamma = gamma;
  out.strategy = lob::paper::strategy_name(strat);
  out.seed = seed;
  for (auto* a : {&out.profit, &out.cash, &out.final_mid, &out.mean_spread}) {
    a->attr("flags").attr("writeable") = false;
  }
  for (auto* a : {&out.final_q, &out.bid_fills, &out.ask_fills}) {
    a->attr("flags").attr("writeable") = false;
  }
  return out;
}

py::dict summarize(const SimResults& r, std::size_t first, std::optional<std::size_t> count) {
  const std::size_t n = r.sims.size();
  if (first > n) {
    throw py::value_error("summarize: first=" + std::to_string(first) + " is past the end (" +
                          std::to_string(n) + " simulations)");
  }
  const std::size_t c = count ? *count : n - first;
  if (c > n - first) {
    throw py::value_error("summarize: first + count exceeds the number of simulations (" +
                          std::to_string(n) + ")");
  }
  const lob::paper::Summary s = lob::paper::summarize(r.sims, first, c);
  return py::dict("n"_a = s.n, "mean_spread"_a = s.mean_spread, "mean_profit"_a = s.mean_profit,
                  "sd_profit"_a = s.sd_profit, "mean_final_q"_a = s.mean_q,
                  "sd_final_q"_a = s.sd_q);
}

std::string fmt_double(double x) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.6g", x);
  return buf;
}

}  // namespace

PYBIND11_MODULE(_lobsim, m) {
  m.doc() =
      "Bindings for the lob::OrderBook engine and the Avellaneda-Stoikov (2008) replication. "
      "Import the public package `lobsim`, not this extension module.";

  // ---- Enums -----------------------------------------------------------------------------------
  py::native_enum<Side>(m, "Side", "enum.IntEnum",
                        "Order side. Values match LOBSTER's direction field: Bid = 1, Ask = -1.")
      .value("Bid", Side::Bid)
      .value("Ask", Side::Ask)
      .finalize();

  py::native_enum<Status>(m, "Status", "enum.Enum",
                          "Outcome of a mutating call. Anything but Ok means the call was "
                          "rejected and the book is unchanged.")
      .value("Ok", Status::Ok)
      .value("InvalidId", Status::InvalidId, "id == 0 (0 is reserved as 'no order')")
      .value("InvalidPrice", Status::InvalidPrice, "price <= 0")
      .value("InvalidQty", Status::InvalidQty, "qty <= 0")
      .value("DuplicateId", Status::DuplicateId, "id is already resting in the book")
      .value("UnknownId", Status::UnknownId, "id is not resting in the book")
      .finalize();

  // ---- Value classes ---------------------------------------------------------------------------
  py::class_<Fill>(m, "Fill",
                   "One trade between a resting order and an aggressor. price is the resting "
                   "order's price; aggressor_id is 0 for execute_resting (external aggressor); "
                   "resting_left is the resting order's size after the fill (0 = gone).")
      .def(py::init([](OrderId resting_id, OrderId aggressor_id, Side resting_side, Price price,
                       Qty qty, Time ts, Qty resting_left) {
             return Fill{resting_id, aggressor_id, resting_side, price, qty, ts, resting_left};
           }),
           py::kw_only(), "resting_id"_a, "aggressor_id"_a, "resting_side"_a, "price"_a, "qty"_a,
           "ts"_a, "resting_left"_a)
      .def_readonly("resting_id", &Fill::resting_id)
      .def_readonly("aggressor_id", &Fill::aggressor_id)
      .def_readonly("resting_side", &Fill::resting_side)
      .def_readonly("price", &Fill::price)
      .def_readonly("qty", &Fill::qty)
      .def_readonly("ts", &Fill::ts)
      .def_readonly("resting_left", &Fill::resting_left)
      .def("__eq__", &fill_eq, py::is_operator())
      .def("__hash__",
           [](const Fill& f) {
             return py::hash(py::make_tuple(f.resting_id, f.aggressor_id,
                                            static_cast<int>(f.resting_side), f.price, f.qty,
                                            f.ts, f.resting_left));
           })
      .def("__repr__", &fill_repr);

  py::class_<Level>(m, "Level",
                    "Aggregated price level: price (ticks), qty (sum of remaining sizes), count "
                    "(number of orders).")
      .def(py::init([](Price price, Qty qty, std::uint32_t count) {
             return Level{price, qty, count};
           }),
           py::kw_only(), "price"_a, "qty"_a, "count"_a)
      .def_readonly("price", &Level::price)
      .def_readonly("qty", &Level::qty)
      .def_readonly("count", &Level::count)
      .def(
          "__eq__",
          [](const Level& a, const Level& b) {
            return a.price == b.price && a.qty == b.qty && a.count == b.count;
          },
          py::is_operator())
      .def("__hash__",
           [](const Level& l) { return py::hash(py::make_tuple(l.price, l.qty, l.count)); })
      .def("__repr__", [](const Level& l) {
        return "Level(price=" + std::to_string(l.price) + ", qty=" + std::to_string(l.qty) +
               ", count=" + std::to_string(l.count) + ")";
      });

  py::class_<OrderInfo>(m, "OrderInfo",
                        "A resting order: id, side, price (ticks), qty (remaining size) and ts "
                        "(time the order took its current queue position).")
      .def(py::init([](OrderId id, Side side, Price price, Qty qty, Time ts) {
             return OrderInfo{id, side, price, qty, ts};
           }),
           py::kw_only(), "id"_a, "side"_a, "price"_a, "qty"_a, "ts"_a)
      .def_readonly("id", &OrderInfo::id)
      .def_readonly("side", &OrderInfo::side)
      .def_readonly("price", &OrderInfo::price)
      .def_readonly("qty", &OrderInfo::qty)
      .def_readonly("ts", &OrderInfo::ts)
      .def(
          "__eq__",
          [](const OrderInfo& a, const OrderInfo& b) {
            return a.id == b.id && a.side == b.side && a.price == b.price && a.qty == b.qty &&
                   a.ts == b.ts;
          },
          py::is_operator())
      .def("__hash__",
           [](const OrderInfo& o) {
             return py::hash(
                 py::make_tuple(o.id, static_cast<int>(o.side), o.price, o.qty, o.ts));
           })
      .def("__repr__", [](const OrderInfo& o) {
        return "OrderInfo(id=" + std::to_string(o.id) + ", side=" + side_name(o.side) +
               ", price=" + std::to_string(o.price) + ", qty=" + std::to_string(o.qty) +
               ", ts=" + std::to_string(o.ts) + ")";
      });

  py::class_<PySubmitResult>(
      m, "SubmitResult",
      "Outcome of a call that can trade: status, filled (size traded by the incoming order, or "
      "executed by execute_resting), resting (size left resting under this id; 0 for market "
      "orders) and fills (tuple of Fill, in execution order, produced by this call only).")
      .def_property_readonly("status", [](const PySubmitResult& s) { return s.r.status; })
      .def_property_readonly("filled", [](const PySubmitResult& s) { return s.r.filled; })
      .def_property_readonly("resting", [](const PySubmitResult& s) { return s.r.resting; })
      .def_property_readonly(
          "fills", [](const PySubmitResult& s) { return fills_tuple(s.fills); },
          "Tuple of Fill produced by this call (a new tuple on each access).")
      .def_property_readonly(
          "ok", [](const PySubmitResult& s) { return s.r.status == Status::Ok; },
          "True if status is Status.Ok.")
      .def("__repr__", [](const PySubmitResult& s) {
        return std::string("SubmitResult(status=Status.") + lob::to_string(s.r.status) +
               ", filled=" + std::to_string(s.r.filled) +
               ", resting=" + std::to_string(s.r.resting) +
               ", fills=<" + std::to_string(s.fills.size()) + " fills>)";
      });

  // ---- Order book ------------------------------------------------------------------------------
  py::class_<PyBook>(
      m, "OrderBook",
      "Single-threaded limit order book with price-time priority (lob::OrderBook).\n\n"
      "Prices are integer ticks, quantities shares, timestamps integer nanoseconds. Matching "
      "mode: submit_limit, submit_market, replace, cancel, reduce. Replay mode (no matching, "
      "the book may cross): add_resting, execute_resting, cancel, reduce. Errors are returned "
      "as Status values, never raised; a rejected call leaves the book unchanged.")
      .def(py::init<>())
      .def(
          "reserve",
          [](PyBook& b, std::size_t max_orders, std::size_t max_levels) {
            b.book().reserve(max_orders, max_levels);
          },
          "max_orders"_a, "max_levels"_a,
          "Preallocate for max_orders resting orders and max_levels levels per side. Does not "
          "change the contents.")
      .def(
          "clear", [](PyBook& b) { b.book().clear(); },
          "Remove every order (keeps allocated capacity).")
      .def("submit_limit", &PyBook::submit_limit, "id"_a, "side"_a, "price"_a, "qty"_a,
           "ts"_a = 0,
           "Limit order. Trades against the opposite side while its best price is at least as "
           "good as price (best price first, oldest first, at the resting price); the remainder "
           "rests at price with timestamp ts. Returns SubmitResult with the fills.")
      .def("submit_market", &PyBook::submit_market, "id"_a, "side"_a, "qty"_a, "ts"_a = 0,
           "Market order: sweeps the opposite side up to qty; the unfilled part is discarded. "
           "The id must not be resting (DuplicateId). Returns SubmitResult with the fills.")
      .def("replace", &PyBook::replace, "id"_a, "new_price"_a, "new_qty"_a, "ts"_a = 0,
           "Change price and/or remaining size. new_qty is the new REMAINING size. Priority "
           "(and timestamp) are kept iff the price is unchanged and new_qty <= remaining; "
           "otherwise the order is pulled and resubmitted at ts, may match (fills carry "
           "aggressor_id = id), and any remainder joins the back of its level.")
      .def(
          "cancel", [](PyBook& b, OrderId id) { return b.book().cancel(id); }, "id"_a,
          "Remove the order entirely. Returns Status.")
      .def(
          "reduce", [](PyBook& b, OrderId id, Qty by_qty) { return b.book().reduce(id, by_qty); },
          "id"_a, "by_qty"_a,
          "Partial cancel that keeps queue position; removes the order if by_qty >= remaining. "
          "Returns Status.")
      .def(
          "add_resting",
          [](PyBook& b, OrderId id, Side side, Price price, Qty qty, Time ts) {
            return b.book().add_resting(id, side, price, qty, ts);
          },
          "id"_a, "side"_a, "price"_a, "qty"_a, "ts"_a = 0,
          "Replay mode: rest an order without matching (the book may lock or cross). Returns "
          "Status.")
      .def("execute_resting", &PyBook::execute_resting, "id"_a, "qty"_a, "ts"_a = 0,
           "Replay mode: execute min(qty, remaining) of a resting order against an external "
           "aggressor. Returns SubmitResult: filled = executed size, resting = size left, fills = "
           "one Fill with aggressor_id 0 (empty if rejected).")
      .def(
          "best", [](const PyBook& b, Side side) { return b.cbook().best(side); }, "side"_a,
          "Best Level of side, or None if that side is empty.")
      .def(
          "best_bid", [](const PyBook& b) { return b.cbook().best_bid(); },
          "Best bid Level, or None.")
      .def(
          "best_ask", [](const PyBook& b) { return b.cbook().best_ask(); },
          "Best ask Level, or None.")
      .def(
          "mid", [](const PyBook& b) { return b.cbook().mid(); },
          "(best bid + best ask) / 2 in ticks as float, or None if either side is empty.")
      .def(
          "spread", [](const PyBook& b) { return b.cbook().spread(); },
          "best ask - best bid in ticks, or None if either side is empty. <= 0 only if the "
          "book is locked or crossed (replay mode).")
      .def(
          "front", [](const PyBook& b, Side side) { return b.cbook().front(side); }, "side"_a,
          "Oldest order at the best price of side (OrderInfo), or None.")
      .def(
          "find", [](const PyBook& b, OrderId id) { return b.cbook().find(id); }, "id"_a,
          "OrderInfo of a resting order, or None.")
      .def(
          "queue_ahead", [](const PyBook& b, OrderId id) { return b.cbook().queue_ahead(id); },
          "id"_a,
          "Total size ahead of id in its price level (0 at the front), or None if id is not "
          "resting. O(position in the level).")
      .def("depth", &PyBook::depth, "side"_a, "n"_a,
           "Best min(n, level_count(side)) levels of side, best first, as a numpy int64 array "
           "of shape (k, 3) with columns [price, qty, count]. n must be >= 0.")
      .def(
          "order_count", [](const PyBook& b) { return b.cbook().order_count(); },
          "Number of resting orders.")
      .def(
          "level_count", [](const PyBook& b, Side side) { return b.cbook().level_count(side); },
          "side"_a, "Number of price levels on side.")
      .def(
          "total_qty", [](const PyBook& b, Side side) { return b.cbook().total_qty(side); },
          "side"_a, "Sum of remaining sizes on side.")
      .def("__len__", [](const PyBook& b) { return b.cbook().order_count(); })
      .def("check_invariants", &PyBook::check_invariants, "require_uncrossed"_a = true,
           "Verify every structural invariant (linear time; for tests). Returns (ok, message); "
           "message is empty when ok. Pass require_uncrossed=False in replay mode.")
      .def("__repr__", &PyBook::repr);

  // ---- Bulk replay -----------------------------------------------------------------------------
  py::class_<ReplayResult>(
      m, "ReplayResult",
      "Outcome of replay_lobster. count_by_type[t] and rejected_by_type[t] (int64 arrays of "
      "length 8) count messages of LOBSTER type t = 1..7; index 0 counts any other type value. "
      "bid_price, bid_qty, ask_price, ask_qty are int64 arrays with the top of book after each "
      "message (price 0 and qty 0 for an empty side) if capture_top was set, otherwise None.")
      .def_readonly("messages", &ReplayResult::messages)
      .def_readonly("rejected", &ReplayResult::rejected)
      .def_readonly("first_rejected", &ReplayResult::first_rejected,
                    "Index of the first rejected message, or -1.")
      .def_readonly("count_by_type", &ReplayResult::count_by_type)
      .def_readonly("rejected_by_type", &ReplayResult::rejected_by_type)
      .def_readonly("bid_price", &ReplayResult::bid_price)
      .def_readonly("bid_qty", &ReplayResult::bid_qty)
      .def_readonly("ask_price", &ReplayResult::ask_price)
      .def_readonly("ask_qty", &ReplayResult::ask_qty)
      .def("__repr__", [](const ReplayResult& r) {
        return "ReplayResult(messages=" + std::to_string(r.messages) +
               ", rejected=" + std::to_string(r.rejected) +
               ", first_rejected=" + std::to_string(r.first_rejected) + ")";
      });

  m.def("replay_lobster", &replay_lobster, "book"_a, "time_ns"_a, "type"_a, "order_id"_a,
        "size"_a, "price"_a, "direction"_a, py::kw_only(), "capture_top"_a = false,
        "Apply LOBSTER-style messages to book in replay mode, in order, without the GIL.\n\n"
        "Columns (1-D numpy arrays of equal length, exact dtypes): time_ns int64, type int8, "
        "order_id uint64, size int64, price int64 (ticks), direction int8.\n"
        "Type 1: add_resting(order_id, Side(direction), price, size, time_ns); a direction "
        "other than +1/-1 is rejected. Type 2: reduce(order_id, size). Type 3: "
        "cancel(order_id). Type 4: execute_resting(order_id, size, time_ns). Types 5, 6, 7: "
        "no change. Any other type value is rejected. A message is rejected when the engine "
        "returns a Status other than Ok; rejected messages leave the book unchanged.\n"
        "Raises TypeError if a column is not a numpy array and ValueError on a wrong dtype, "
        "a non-1-D array or mismatched lengths.");

  // ---- Paper -----------------------------------------------------------------------------------
  py::module_ mp = m.def_submodule(
      "paper", "Avellaneda and Stoikov (2008) simulation (lob::paper::run_experiment).");

  py::class_<SimResults>(
      mp, "SimResults",
      "Per-simulation results of paper.simulate as read-only numpy arrays (one entry per "
      "simulation): profit, cash, final_mid, mean_spread (float64) and final_q, bid_fills, "
      "ask_fills (int64). Also gamma, strategy and seed.")
      .def_readonly("profit", &SimResults::profit)
      .def_readonly("final_q", &SimResults::final_q)
      .def_readonly("mean_spread", &SimResults::mean_spread)
      .def_readonly("cash", &SimResults::cash)
      .def_readonly("final_mid", &SimResults::final_mid)
      .def_readonly("bid_fills", &SimResults::bid_fills)
      .def_readonly("ask_fills", &SimResults::ask_fills)
      .def_readonly("gamma", &SimResults::gamma)
      .def_readonly("strategy", &SimResults::strategy)
      .def_readonly("seed", &SimResults::seed)
      .def("__len__", [](const SimResults& r) { return r.sims.size(); })
      .def("__repr__", [](const SimResults& r) {
        return "SimResults(gamma=" + fmt_double(r.gamma) + ", strategy='" + r.strategy +
               "', seed=" + std::to_string(r.seed) + ", sims=" + std::to_string(r.sims.size()) +
               ")";
      });

  mp.def("simulate", &simulate, "gamma"_a, "sims"_a, "seed"_a, "strategy"_a = "inventory",
         py::kw_only(), "s0"_a = 100.0, "sigma"_a = 2.0, "horizon"_a = 1.0, "A"_a = 140.0,
         "k"_a = 1.5, "steps"_a = 200,
         "Run `sims` simulations of one strategy at risk aversion gamma, without the GIL.\n\n"
         "Simulation i uses the random path Rng::for_stream(seed, i), the same as the C++ "
         "paper_replication tool, so results agree with results/paper/ for the same seed. "
         "strategy is 'inventory', 'symmetric' or 'symmetric_tv'. Model keyword arguments "
         "default to the paper's values (s0=100, sigma=2, horizon=1, A=140, k=1.5, steps=200). "
         "Raises ValueError on invalid parameters. Returns SimResults.");

  mp.def("summarize", &summarize, "results"_a, "first"_a = 0, "count"_a = py::none(),
         "Summary of results[first:first+count] via lob::paper::summarize: a dict with n, "
         "mean_spread, mean_profit, sd_profit, mean_final_q, sd_final_q (sample standard "
         "deviations, divisor n - 1; 0 when n < 2). Keys match results/paper/*/summary.csv.");

  mp.attr("STRATEGIES") = py::make_tuple("inventory", "symmetric", "symmetric_tv");
}
