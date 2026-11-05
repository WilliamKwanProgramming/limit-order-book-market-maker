add_library(lob_backtest
  src/params.cpp
  src/hawkes.cpp
  src/strategy.cpp
  src/market.cpp
  src/experiment.cpp)
target_include_directories(lob_backtest PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lob_backtest PUBLIC lob::core lob::paper)
add_library(lob::backtest ALIAS lob_backtest)

foreach(exe backtest_run stress_test)
  add_executable(${exe} src/${exe}.cpp)
  target_link_libraries(${exe} PRIVATE lob::backtest)
endforeach()

if(LOB_BUILD_TESTS)
  foreach(test test_strategy test_hawkes test_market test_experiment)
    add_executable(backtest_${test} tests/${test}.cpp)
    target_link_libraries(backtest_${test} PRIVATE lob::backtest)
    add_test(NAME backtest_${test} COMMAND backtest_${test})
  endforeach()
  # The simulation-heavy tests run many episodes. Optimize these test translation units in Debug
  # builds (sanitizers stay on); lob_backtest itself keeps the build type's flags.
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    foreach(t backtest_test_hawkes backtest_test_market backtest_test_experiment)
      target_compile_options(${t} PRIVATE $<$<CONFIG:Debug>:-O1>)
    endforeach()
  endif()

  # Smoke tests of the two command-line tools.
  add_test(NAME backtest_cli_smoke
    COMMAND backtest_run --episodes 3 --horizon 30 --burn-in 30 --bootstrap 20 --calibrate 1
            --strategies none,naive,as:0.01,sym:0.01 --as-sigma 0.5 --as-k 1.5
            --out ${CMAKE_CURRENT_BINARY_DIR}/cli_smoke)
  add_test(NAME backtest_stress_smoke
    COMMAND stress_test --episodes 2 --horizon 30 --burn-in 30 --bootstrap 10
            --nu-grid 0,0.1 --nbr-grid 0.5 --strategies naive,as:0.01,sym:0.01
            --as-sigma 0.5 --as-k 1.5 --out ${CMAKE_CURRENT_BINARY_DIR}/stress_smoke)
endif()
