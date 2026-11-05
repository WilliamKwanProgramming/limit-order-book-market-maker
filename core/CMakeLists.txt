option(LOB_BUILD_BENCH "Build the core benchmark executable bench_replay" ON)
option(LOB_CORE_MAP_LEVELS
       "Use std::map for price levels (benchmark baseline only; default is a sorted vector)" OFF)

add_library(lob_core src/order_book.cpp)
target_include_directories(lob_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lob_core PUBLIC lob::common)
if(LOB_CORE_MAP_LEVELS)
  # PUBLIC: the macro changes OrderBook's layout, so every user must see the same value.
  target_compile_definitions(lob_core PUBLIC LOB_CORE_MAP_LEVELS)
endif()
add_library(lob::core ALIAS lob_core)

if(LOB_BUILD_TESTS)
  foreach(test test_order_book test_matching_differential test_replay_differential)
    add_executable(core_${test} tests/${test}.cpp)
    target_link_libraries(core_${test} PRIVATE lob::core)
    add_test(NAME core_${test} COMMAND core_${test})
  endforeach()
endif()

if(LOB_BUILD_BENCH)
  add_executable(bench_replay bench/bench_replay.cpp)
  target_link_libraries(bench_replay PRIVATE lob::core)
  # Record what was compiled so the benchmark output is self-describing.
  string(TOUPPER "${CMAKE_BUILD_TYPE}" lob_build_type_upper)
  set(lob_bench_flags "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${lob_build_type_upper}}")
  if(LOB_NATIVE)
    string(APPEND lob_bench_flags " -march=native")
  endif()
  if(LOB_SANITIZE)
    string(APPEND lob_bench_flags " -fsanitize=address,undefined")
  endif()
  string(STRIP "${lob_bench_flags}" lob_bench_flags)
  if(LOB_CORE_MAP_LEVELS)
    set(lob_bench_levels "std::map (LOB_CORE_MAP_LEVELS=ON)")
  else()
    set(lob_bench_levels "sorted std::vector, best at back (default)")
  endif()
  target_compile_definitions(bench_replay PRIVATE
    LOB_BENCH_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
    LOB_BENCH_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
    LOB_BENCH_FLAGS="${lob_bench_flags}"
    LOB_BENCH_LEVELS="${lob_bench_levels}")
endif()
