add_library(lob_lobster
  src/format.cpp
  src/reader.cpp
  src/replayer.cpp
  src/validator.cpp
  src/stats.cpp
  src/session.cpp)
target_include_directories(lob_lobster PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lob_lobster PUBLIC lob::core)
add_library(lob::lobster ALIAS lob_lobster)

add_executable(lobster_replay src/lobster_replay_main.cpp)
target_link_libraries(lobster_replay PRIVATE lob::lobster)
# Record what was compiled so --bench output is self-describing.
string(TOUPPER "${CMAKE_BUILD_TYPE}" lobster_build_type_upper)
set(lobster_flags "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${lobster_build_type_upper}}")
if(LOB_NATIVE)
  string(APPEND lobster_flags " -march=native")
endif()
if(LOB_SANITIZE)
  string(APPEND lobster_flags " -fsanitize=address,undefined")
endif()
string(STRIP "${lobster_flags}" lobster_flags)
set(lobster_build_info
    "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}, ${CMAKE_BUILD_TYPE}, ${lobster_flags}")
target_compile_definitions(lobster_replay PRIVATE LOBSTER_BUILD_INFO="${lobster_build_info}")

if(LOB_BUILD_TESTS)
  foreach(test test_format test_replay test_alloc)
    add_executable(lobster_${test} tests/${test}.cpp)
    target_link_libraries(lobster_${test} PRIVATE lob::lobster)
    add_test(NAME lobster_${test} COMMAND lobster_${test})
  endforeach()
  # The randomized parser test runs many iterations; keep it fast in Debug/sanitizer builds.
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(lobster_test_format PRIVATE $<$<CONFIG:Debug>:-O1>)
  endif()

  # Cross-implementation test against the independent Python generator and reference book.
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND)
    add_test(NAME lobster_integration
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration_test.py
              --replay $<TARGET_FILE:lobster_replay>
              --synth ${CMAKE_CURRENT_SOURCE_DIR}/tools/synth_lobster.py
              --workdir ${CMAKE_CURRENT_BINARY_DIR}/integration)
  else()
    add_executable(lobster_skip tests/skip_main.cpp)
    add_test(NAME lobster_integration COMMAND lobster_skip "python3 not found")
  endif()
  set_tests_properties(lobster_integration PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
endif()
