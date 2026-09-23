# Force the GNU C compiler for every configuration.
#
# The project explicitly refuses clang (see the guard in the top-level
# CMakeLists.txt). Homebrew installs GNU C under versioned names; probe the
# newest first so a future `brew install gcc` upgrade keeps working without
# touching this file.
find_program(DODPLACE_GCC_EXECUTABLE
    NAMES gcc-17 gcc-16 gcc-15 gcc-14 gcc-13
    DOC "GNU C compiler (Homebrew gcc-<version>)")

if(NOT DODPLACE_GCC_EXECUTABLE)
    message(FATAL_ERROR
        "No GNU C compiler found (looked for gcc-17..gcc-13).\n"
        "Install it with:  brew install gcc")
endif()

set(CMAKE_C_COMPILER "${DODPLACE_GCC_EXECUTABLE}" CACHE FILEPATH "GNU C compiler" FORCE)
