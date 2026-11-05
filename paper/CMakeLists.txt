add_library(lob_paper
  src/avellaneda_stoikov.cpp
  src/paper_experiment.cpp)
target_include_directories(lob_paper PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lob_paper PUBLIC lob::common)
add_library(lob::paper ALIAS lob_paper)

add_executable(paper_replication src/main.cpp)
target_link_libraries(paper_replication PRIVATE lob::paper)

if(LOB_BUILD_TESTS)
  add_executable(paper_tests tests/test_paper.cpp)
  target_link_libraries(paper_tests PRIVATE lob::paper)
  add_test(NAME paper_tests COMMAND paper_tests)

  # Smoke test of the CLI: a tiny run must succeed and write its outputs.
  add_test(NAME paper_cli_smoke
    COMMAND paper_replication --sims 50 --bootstrap 20 --block-size 10 --trace 3
            --out ${CMAKE_CURRENT_BINARY_DIR}/cli_smoke)
endif()
