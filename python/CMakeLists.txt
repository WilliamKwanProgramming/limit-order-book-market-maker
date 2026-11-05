# Python bindings: the pybind11 extension lobsim._lobsim plus the pure-Python package python/lobsim.
#
# Optional. Configuration never fails because of this directory: the module is skipped with a
# status message when Python development headers, pybind11 (>= 3.0), or the core/paper modules are
# missing, or when LOB_SANITIZE=ON (the Python interpreter is not ASan-instrumented).
#
# The importable package is assembled in ${CMAKE_CURRENT_BINARY_DIR}/lobsim, so
#   PYTHONPATH=<build>/python python3 -c "import lobsim"
# works after `cmake --build <build> --target lobsim`.

set(lobsim_skip "")
if(LOB_SANITIZE)
  set(lobsim_skip "LOB_SANITIZE=ON (Python is not ASan-instrumented)")
elseif(NOT "core" IN_LIST LOB_MODULES OR NOT "paper" IN_LIST LOB_MODULES)
  set(lobsim_skip "it needs the core and paper modules in LOB_MODULES")
endif()

if(NOT lobsim_skip)
  find_package(Python3 COMPONENTS Interpreter Development.Module QUIET)
  if(NOT Python3_FOUND)
    set(lobsim_skip "Python3 interpreter or development headers not found")
  endif()
endif()

if(NOT lobsim_skip AND NOT pybind11_DIR)
  execute_process(
    COMMAND "${Python3_EXECUTABLE}" -m pybind11 --cmakedir
    OUTPUT_VARIABLE lobsim_pybind11_cmakedir
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE lobsim_pybind11_rc)
  if(lobsim_pybind11_rc EQUAL 0 AND lobsim_pybind11_cmakedir)
    set(pybind11_DIR "${lobsim_pybind11_cmakedir}" CACHE PATH "pybind11 CMake config directory")
  endif()
endif()

if(NOT lobsim_skip)
  find_package(pybind11 3.0 CONFIG QUIET)
  if(NOT pybind11_FOUND)
    set(lobsim_skip "pybind11 >= 3.0 not found (pip install pybind11)")
  endif()
endif()

if(lobsim_skip)
  message(STATUS "lobsim (python/): skipped, ${lobsim_skip}")
  return()
endif()
message(STATUS "lobsim (python/): Python ${Python3_VERSION} at ${Python3_EXECUTABLE}, "
               "pybind11 ${pybind11_VERSION}")

set(lobsim_pkg_dir "${CMAKE_CURRENT_BINARY_DIR}/lobsim")

pybind11_add_module(lobsim MODULE src/lobsim.cpp)
set_target_properties(lobsim PROPERTIES
  OUTPUT_NAME _lobsim
  LIBRARY_OUTPUT_DIRECTORY "${lobsim_pkg_dir}")
# pybind11's own and Python's headers are imported targets, so they are included as system
# headers and the repository's warning flags apply only to our code.
target_link_libraries(lobsim PRIVATE lob::core lob::paper lob::options)

# The static libraries end up in a shared object.
set_target_properties(lob_core lob_paper PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Pure-Python part and type stubs, copied next to the extension (re-copied on change).
set(lobsim_py_files __init__.py paper.py __init__.pyi paper.pyi py.typed)
foreach(f IN LISTS lobsim_py_files)
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/lobsim/${f}" "${lobsim_pkg_dir}/${f}" COPYONLY)
endforeach()

if(SKBUILD)
  # pip route (python/pyproject.toml, scikit-build-core).
  install(TARGETS lobsim LIBRARY DESTINATION lobsim)
  foreach(f IN LISTS lobsim_py_files)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/lobsim/${f}" DESTINATION lobsim)
  endforeach()
endif()

if(LOB_BUILD_TESTS)
  # Exit code 77 (pytest or the module not importable) is reported as "skipped", not failed.
  add_test(NAME python_pytest
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_pytest.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests" -q)
  set_tests_properties(python_pytest PROPERTIES
    ENVIRONMENT "PYTHONPATH=${CMAKE_CURRENT_BINARY_DIR};LOB_REPO_ROOT=${PROJECT_SOURCE_DIR}"
    SKIP_RETURN_CODE 77
    TIMEOUT 1200)
endif()
