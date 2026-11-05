// Stand-in for lobster_integration when CMake found no Python 3 interpreter: CTest reports the
// test as skipped (SKIP_RETURN_CODE 77).
#include <cstdio>

int main(int argc, char** argv) {
  std::printf("skipped: %s\n", argc > 1 ? argv[1] : "requirement missing");
  return 77;
}
