# =============================================================================
# analyze/CMakeLists.txt
#
# Builds `rpg-analyze`, a static-analysis CLI for RPG II source. It reuses the
# compiler's frontend (rpgc_parse, from ../compiler) and never links LLVM.
# =============================================================================

file(GLOB RPGANALYZE_MODULE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/modules/*.cpp)
file(GLOB RPGANALYZE_CMD_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/cmds/*.cpp)

add_executable(rpg-analyze
    src/main.cpp
    src/ir.cpp
    src/finding.cpp
    src/json.cpp
    src/report.cpp
    src/registry.cpp
    src/render_text.cpp
    src/render_json.cpp
    ${RPGANALYZE_MODULE_SOURCES}
    ${RPGANALYZE_CMD_SOURCES}
)

target_include_directories(rpg-analyze PRIVATE
    ${CMAKE_SOURCE_DIR}/compiler/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(rpg-analyze PRIVATE rpgc_parse)
