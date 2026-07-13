# =============================================================================
# clean/CMakeLists.txt
#
# Builds `rpg-clean`, a standalone source-cleanup/normalization CLI. It reuses
# the cleanup pipeline from rpgc_parse (compiler/src/clean.cpp) and never links
# LLVM or the runtime -- it is pure byte-level text munging. Mirrors the
# analyze/ build shape.
# =============================================================================

add_executable(rpg-clean
    src/main.cpp
)

target_include_directories(rpg-clean PRIVATE
    ${CMAKE_SOURCE_DIR}/compiler/src
)

target_link_libraries(rpg-clean PRIVATE rpgc_parse)

install(TARGETS rpg-clean
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
