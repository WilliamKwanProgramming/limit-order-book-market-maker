#include "lob/lobster_validator.hpp"

namespace lob::lobster {

Validator::Validator(std::size_t levels) {
  report_.levels = levels;
  report_.ask_level_mismatches.assign(levels, 0);
  report_.bid_level_mismatches.assign(levels, 0);
  depth_.reserve(levels);
  actual_.resize(levels);
  prev_.resize(levels);
}

void Validator::engine_row(const OrderBook& book, BookRow& out) {
  const std::size_t n = report_.levels;
  const auto fill = [&](Side side, std::vector<BookLevel>& lv) {
    book.depth(side, n, depth_);
    for (std::size_t i = 0; i < n; ++i) {
      lv[i] = i < depth_.size() ? BookLevel{depth_[i].price, depth_[i].qty} : BookLevel{};
    }
  };
  fill(Side::Ask, out.asks);
  fill(Side::Bid, out.bids);
}

bool Validator::check(const OrderBook& book, const Message& m, std::uint64_t index,
                      std::uint64_t line, const BookRow& row) {
  const std::size_t n = report_.levels;
  if (!changes_visible_book(m.type) && have_prev_) {
    ++report_.unchanged_checked;
    if (!(row == prev_)) {
      if (report_.unchanged_violations == 0) report_.first_unchanged_violation_index = index;
      ++report_.unchanged_violations;
    }
  }
  prev_.asks.assign(row.asks.begin(), row.asks.end());
  prev_.bids.assign(row.bids.begin(), row.bids.end());
  have_prev_ = true;

  engine_row(book, actual_);
  ++report_.checked;
  bool any = false;
  bool structure = false;
  for (std::size_t i = 0; i < n; ++i) {
    const BookLevel& ea = row.asks[i];
    const BookLevel& aa = actual_.asks[i];
    if (ea != aa) {
      any = true;
      ++report_.ask_level_mismatches[i];
      if (ea.price != aa.price || ea.empty() != aa.empty()) structure = true;
    }
    const BookLevel& eb = row.bids[i];
    const BookLevel& ab = actual_.bids[i];
    if (eb != ab) {
      any = true;
      ++report_.bid_level_mismatches[i];
      if (eb.price != ab.price || eb.empty() != ab.empty()) structure = true;
    }
  }
  if (!any) return true;
  ++report_.mismatched;
  if (structure) {
    ++report_.price_structure;
  } else {
    ++report_.size_only;
  }
  if (!report_.first.set) {
    report_.first.set = true;
    report_.first.index = index;
    report_.first.line = line;
    report_.first.message = m;
    report_.first.expected = row;
    report_.first.actual = actual_;
  }
  return false;
}

void Validator::skip(const BookRow* row) {
  ++report_.skipped;
  if (row != nullptr && row->levels() == report_.levels) {
    prev_.asks.assign(row->asks.begin(), row->asks.end());
    prev_.bids.assign(row->bids.begin(), row->bids.end());
    have_prev_ = true;
  } else {
    have_prev_ = false;
  }
}

}  // namespace lob::lobster
