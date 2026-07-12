# =============================================================================
# runtime/CMakeLists.txt
#
# Builds `librpgruntime.a`, the small C runtime the generated RPG programs are
# linked against. In Phase 1 it's a stub; later phases add file I/O and the
# built-in RPG cycle helpers.
# =============================================================================

add_library(rpgruntime STATIC
    rpg_runtime.c
)

# Target name should be `librpgruntime.a` per the prompt; the `lib` prefix is
# added automatically by CMake for STATIC libraries on Unix, and the base name
# here is "rpgruntime", so the output file is librpgruntime.a. Good.
target_include_directories(rpgruntime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# The compiler binary needs to know where the built .a lives so it can pass it
# to the linker when it shells out to clang.
# (No extra defines needed in Phase 1; placeholder for later phases.)

# -----------------------------------------------------------------------------
# Install
#
# librpgruntime.a goes under a `rpgc/` subdirectory of the normal lib dir
# (not directly in lib/) so that an installed `rpgc` can find it relative to
# its own path: <prefix>/bin/rpgc looks for <prefix>/lib/rpgc/librpgruntime.a.
# See compiler/src/main.cpp's default_runtime_lib().
# -----------------------------------------------------------------------------
install(TARGETS rpgruntime
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}/rpgc
)
install(FILES rpg_runtime.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/rpgc
)
