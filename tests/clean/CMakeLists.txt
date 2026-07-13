# =============================================================================
# tests/clean/CMakeLists.txt
#
# Builds `test_clean`, a unit-test driver for the cleanup pipeline in
# compiler/src/clean.cpp. It links rpgc_parse directly and asserts on the
# per-stage detectors + the full pipeline. Gated behind BUILD_TESTING (off by
# default) so it never enters the normal/package build:
#
#     cmake -S . -B build -DBUILD_TESTING=ON
#     cmake --build build --target test_clean -j
#     ./build/tests/clean/test_clean
# =============================================================================
add_executable(test_clean test_clean.cpp)

target_include_directories(test_clean PRIVATE
    ${CMAKE_SOURCE_DIR}/compiler/src
)

target_link_libraries(test_clean PRIVATE rpgc_parse)
