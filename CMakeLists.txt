cmake_minimum_required(VERSION 3.16)
project(limit_order_book_market_maker VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(LOB_BUILD_TESTS "Build and register tests" ON)
option(LOB_WERROR "Treat compiler warnings as errors" OFF)
option(LOB_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(LOB_NATIVE "Compile with -march=native (local experiments only; not used for published numbers)" OFF)

# Flags shared by every target: link against lob::options.
add_library(lob_options INTERFACE)
target_compile_features(lob_options INTERFACE cxx_std_17)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(lob_options INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
  if(LOB_WERROR)
    target_compile_options(lob_options INTERFACE -Werror)
  endif()
  if(LOB_SANITIZE)
    target_compile_options(lob_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(lob_options INTERFACE -fsanitize=address,undefined)
  endif()
  if(LOB_NATIVE)
    target_compile_options(lob_options INTERFACE -march=native)
  endif()
elseif(MSVC)
  target_compile_options(lob_options INTERFACE /W4 /permissive-)
endif()
add_library(lob::options ALIAS lob_options)

if(LOB_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(common)

# Each module owns its directory and CMakeLists.txt; it is picked up when present. Configure a
# subset with, for example, -DLOB_MODULES="core;paper" (modules a target depends on must be listed).
set(LOB_MODULES "core;paper;lobster;backtest;python" CACHE STRING
    "Semicolon-separated list of modules to configure")
foreach(module IN LISTS LOB_MODULES)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${module}/CMakeLists.txt")
    add_subdirectory(${module})
  endif()
endforeach()
