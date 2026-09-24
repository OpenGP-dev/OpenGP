cmake_minimum_required(VERSION 3.24)
project(OpenGP LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(OPENGP_BUILD_TESTS "Build the test suite" ON)

if(MSVC AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  message(FATAL_ERROR "OpenGP needs GCC or Clang (use clang-cl on Windows): "
                      "recompiled code relies on weak symbols, which MSVC does not support.")
endif()

# Settings shared by everything that executes guest code (the runtime, the
# interpreter and the generated code). Floating-point contraction must be off:
# the host compiler must not fuse or reorder guest FP operations, or recompiled
# code and the interpreter could produce different bits.
add_library(opengp_guest_flags INTERFACE)
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
  target_compile_options(opengp_guest_flags INTERFACE
    /clang:-ffp-contract=off /clang:-fwrapv /clang:-fno-strict-aliasing)
else()
  target_compile_options(opengp_guest_flags INTERFACE
    -ffp-contract=off -fwrapv -fno-strict-aliasing)
endif()

add_library(opengp_warnings INTERFACE)
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
  target_compile_options(opengp_warnings INTERFACE /W4)
else()
  target_compile_options(opengp_warnings INTERFACE -Wall -Wextra)
endif()

add_library(tomlplusplus INTERFACE)
target_include_directories(tomlplusplus SYSTEM INTERFACE thirdparty/tomlplusplus)

add_library(doctest INTERFACE)
target_include_directories(doctest SYSTEM INTERFACE thirdparty/doctest)

add_subdirectory(common/gekko)
add_subdirectory(runtime)
add_subdirectory(tools/gekkorecomp)

if(OPENGP_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
