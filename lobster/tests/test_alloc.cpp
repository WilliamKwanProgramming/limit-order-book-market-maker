// Steady-state allocation check: a full session (parse, replay, validate, stats) over 2,000 and
// over 20,000 message cycles must perform the same number of heap allocations, i.e. none per
// message once the buffers exist. Global operator new is replaced to count allocations.
#include <atomic>
#include <cstdlib>
#include <new>
#include <string>

#include "lob/check.hpp"
#include "lob/lobster_session.hpp"

namespace {
std::atomic<long> g_allocations{0};
}

void* operator new(std::size_t n) {
  g_allocations.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n == 0 ? 1 : n)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

using namespace lob::lobster;

namespace {

// Each cycle: add a bid, execute part of it, add an ask, halt row, delete both. One level.
void make_files(int cycles, std::string& msgs, std::string& book) {
  long long id = 1000;
  long long t = 34200;
  for (int i = 0; i < cycles; ++i, ++t) {
    const std::string ts = std::to_string(t) + "." + std::to_string(100000000 + i % 900000000);
    const std::string bid = std::to_string(id);
    const std::string ask = std::to_string(id + 3);
    const std::string px = std::to_string(1000000 + 100 * (i % 7));
    const std::string apx = std::to_string(1001000 + 100 * (i % 5));
    msgs += ts + ",1," + bid + ",100," + px + ",1\n";
    book += "9999999999,0," + px + ",100\n";
    msgs += ts + ",4," + bid + ",40," + px + ",1\n";
    book += "9999999999,0," + px + ",60\n";
    msgs += ts + ",1," + ask + ",30," + apx + ",-1\n";
    book += apx + ",30," + px + ",60\n";
    msgs += ts + ",7,0,0,-1,0\n";
    book += apx + ",30," + px + ",60\n";
    msgs += ts + ",3," + bid + ",60," + px + ",1\n";
    book += apx + ",30,-9999999999,0\n";
    msgs += ts + ",3," + ask + ",30," + apx + ",-1\n";
    book += "9999999999,0,-9999999999,0\n";
    id += 7;
  }
}

long allocations_for(int cycles, SessionResult& out) {
  std::string msgs;
  std::string book;
  make_files(cycles, msgs, book);
  SessionOptions opt;
  opt.stats = true;
  StringSource ms(msgs);
  StringSource bs(book);
  const long before = g_allocations.load();
  out = run_session(ms, &bs, opt);
  return g_allocations.load() - before;
}

}  // namespace

LOB_TEST(no_allocation_per_message) {
  SessionResult small;
  SessionResult large;
  const long a_small = allocations_for(2000, small);
  const long a_large = allocations_for(20000, large);
  LOB_CHECK_EQ(small.exit_code(), 0);
  LOB_CHECK_EQ(large.exit_code(), 0);
  LOB_CHECK_EQ(large.validation.checked, std::uint64_t{120000});
  LOB_CHECK_EQ(large.validation.mismatched, std::uint64_t{0});
  LOB_CHECK_EQ(a_small, a_large);
  std::printf("  allocations per session: %ld (2,000 cycles), %ld (20,000 cycles)\n", a_small,
              a_large);
}

LOB_TEST_MAIN()
