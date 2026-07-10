# =============================================================================
# RPG II -> LLVM Compiler
# Top-level CMakeLists.txt
#
# This project builds:
#   1. `rpgc`          - the compiler executable (C++, links against libLLVM)
#   2. `librpgruntime` - a static C runtime library for file I/O & built-ins
#
# LLVM is located through `llvm-config-19` (or a fallback `llvm-config`), which
# gives us the correct include dirs, library list, and definition flags for the
# version actually installed on the machine.
# =============================================================================

cmake_minimum_required(VERSION 3.16)

project(rpg2_compiler
    VERSION 0.1.0
    LANGUAGES C CXX
)

# -----------------------------------------------------------------------------
# Global build settings
# -----------------------------------------------------------------------------
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# RTTI must be OFF and exceptions OFF to match how LLVM is built; otherwise
# linking against libLLVM produces ABI errors on many distros.
if(NOT MSVC)
    add_compile_options(
        $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    )
endif()

# Default to a Release-style build with debug info if the user said nothing.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Output everything into one bin/ dir for convenience.
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# -----------------------------------------------------------------------------
# Locate LLVM via llvm-config-19 (preferred), then plain llvm-config.
# -----------------------------------------------------------------------------
find_program(LLVM_CONFIG_BIN
    NAMES llvm-config-19 llvm-config-19.0 llvm-config
    DOC "Path to llvm-config for the LLVM we build against"
)
if(LLVM_CONFIG_BIN STREQUAL "LLVM_CONFIG_BIN-NOTFOUND")
    message(FATAL_ERROR
        "Could not find llvm-config. Install llvm-19-dev (e.g.\n"
        "  sudo apt-get install -y llvm-19-dev clang-19)")
endif()

message(STATUS "Using llvm-config: ${LLVM_CONFIG_BIN}")

# Query the bits we need. OUTPUT_VARIABLE gives us the raw stdout; we strip the
# trailing newline and convert to a CMake list (semicolon-separated).
execute_process(
    COMMAND ${LLVM_CONFIG_BIN} --version
    OUTPUT_VARIABLE LLVM_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(
    COMMAND ${LLVM_CONFIG_BIN} --includedir
    OUTPUT_VARIABLE LLVM_INCLUDE_DIRS
    OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(
    COMMAND ${LLVM_CONFIG_BIN} --libdir
    OUTPUT_VARIABLE LLVM_LIB_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(
    COMMAND ${LLVM_CONFIG_BIN} --cmakedir
    OUTPUT_VARIABLE LLVM_CMAKE_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(
    COMMAND ${LLVM_CONFIG_BIN} --system-libs
    OUTPUT_VARIABLE LLVM_SYSTEM_LIBS
    OUTPUT_STRIP_TRAILING_WHITESPACE)

message(STATUS "LLVM version: ${LLVM_VERSION}")

# The modern, robust way to link LLVM from CMake is to use the CMake package
# that ships with LLVM via `--cmakedir`. This brings the llvm_map_components...
# helper and proper imported targets.
list(APPEND CMAKE_PREFIX_PATH "${LLVM_CMAKE_DIR}")

# Now find the LLVM CMake package. This defines the `llvm_map_components_to_libnames`
# function and the `LLVM` and `LLVMConfig` targets.
find_package(LLVM ${LLVM_VERSION} EXACT REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

# -----------------------------------------------------------------------------
# A convenience wrapper around the `llc` / `clang` we'll invoke at runtime
# (Phase 5 driver logic). We expose them as cache variables so the compiler
# binary can default to the same toolchain it was built with.
# -----------------------------------------------------------------------------
find_program(LLVM_LLC  NAMES llc-19 llc)
find_program(LLVM_CLANG NAMES clang-19 clang)
set(RPGC_LLC   "${LLVM_LLC}"   CACHE STRING "Path to llc used by rpgc")
set(RPGC_CLANG "${LLVM_CLANG}" CACHE STRING "Path to clang used by rpgc")

# -----------------------------------------------------------------------------
# Subdirectories
# -----------------------------------------------------------------------------
add_subdirectory(runtime)
add_subdirectory(compiler)
