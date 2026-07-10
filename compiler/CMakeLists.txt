# =============================================================================
# compiler/CMakeLists.txt
#
# Builds the `rpgc` executable. It links against LLVM (via the CMake package
# located in the top-level CMakeLists.txt) and against the runtime library.
# =============================================================================

add_executable(rpgc
    src/main.cpp
    src/diagnostics.cpp
    src/source.cpp
    src/cspec.cpp
    src/fspec.cpp
    src/ispec.cpp
    src/ospec.cpp
    src/espec.cpp
    src/symbols.cpp
    src/codegen.cpp
    src/driver.cpp
)

# --- LLVM linkage ------------------------------------------------------------
# llvm_map_components_to_libnames turns the symbolic component list into the
# concrete libLLVM*.a / libLLVM-19.so entries for the installed LLVM.
# We pull in exactly the components the frontend needs:
#   core      - Module, Function, BasicBlock, IR types
#   support   - string handling, error propagation
#   irreader  - parse textual .ll (handy for testing)
#   analysis  - the verifier pass
#   codegen   - emit object code / asm
#   target    + native - the host target backend (x86-64 on this machine)
#   passes    - the new pass-manager infrastructure for optimization
llvm_map_components_to_libnames(LLVM_LIBS
    core
    support
    irreader
    analysis
    codegen
    target
    native
    passes
)

target_include_directories(rpgc PRIVATE
    ${LLVM_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(rpgc PRIVATE
    ${LLVM_LIBS}
    rpgruntime          # so rpgc itself can sanity-check the runtime ABI
)

# Tell the binary which llc / clang to invoke when it drives the back end.
# The values come from cache variables set at the top level.
target_compile_definitions(rpgc PRIVATE
    RPGC_DEFAULT_LLC="${RPGC_LLC}"
    RPGC_DEFAULT_CLANG="${RPGC_CLANG}"
    RPGC_RUNTIME_LIB="$<TARGET_FILE:rpgruntime>"
    RPGC_RUNTIME_INC="${CMAKE_SOURCE_DIR}/runtime"
)
