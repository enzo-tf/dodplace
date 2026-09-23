cmake_minimum_required(VERSION 3.25)

# ---------------------------------------------------------------------------
# Compiler policy: GNU C only.
#
# `cc` / `gcc` on macOS are shims for Apple clang. To make a bare
# `cmake -S . -B build` work anyway, probe for a Homebrew GNU C compiler
# before `project()` runs; an explicit -DCMAKE_C_COMPILER or a toolchain file
# always wins. clang is then refused unless DODPLACE_ALLOW_CLANG=ON.
# ---------------------------------------------------------------------------
if(NOT DEFINED CMAKE_C_COMPILER AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    find_program(_dodplace_gcc NAMES gcc-17 gcc-16 gcc-15 gcc-14 gcc-13)
    if(_dodplace_gcc)
        set(CMAKE_C_COMPILER "${_dodplace_gcc}" CACHE FILEPATH "GNU C compiler")
    endif()
endif()

project(dodplace VERSION 0.1.0 LANGUAGES C)

option(DODPLACE_ALLOW_CLANG "Allow configuring with clang (unsupported)" OFF)
# Note: Apple's toolchain reports "AppleClang", not "Clang".
if(CMAKE_C_COMPILER_ID MATCHES "^(Apple)?Clang$" AND NOT DODPLACE_ALLOW_CLANG)
    message(FATAL_ERROR
        "clang is refused by this project (DODPLACE_ALLOW_CLANG=OFF).\n"
        "Configure with:  cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=${CMAKE_SOURCE_DIR}/cmake/toolchain-gcc.cmake")
endif()

# --- Language level ---------------------------------------------------------
# C23 with extensions OFF -> strict `-std=c23`: no GNU dialect leaked in by
# accident, so what compiles here is portable C23.
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# --- Options ----------------------------------------------------------------
option(DODPLACE_WERROR   "Treat every warning as an error"      ON)
option(DODPLACE_SANITIZE "ASan+UBSan in Debug configuration"    ON)
option(DODPLACE_ANALYZE  "Enable GCC -fanalyzer static analysis" OFF)

# --- Shared compile flags ---------------------------------------------------
add_library(dodplace_flags INTERFACE)
target_compile_options(dodplace_flags INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -Wsign-conversion
    -Wcast-align
    -Wstrict-prototypes
    -Wmissing-prototypes
    -Wold-style-definition
    -Wundef
    -Wpointer-arith
    -Wwrite-strings
    -Wvla
    -Wdouble-promotion
    -Wformat=2
    -Wno-format-nonliteral
    $<$<BOOL:${DODPLACE_WERROR}>:-Werror>
    $<$<BOOL:${DODPLACE_ANALYZE}>:-fanalyzer>
    $<$<AND:$<BOOL:${DODPLACE_SANITIZE}>,$<CONFIG:Debug>>:-fsanitize=address,undefined>
    $<$<AND:$<BOOL:${DODPLACE_SANITIZE}>,$<CONFIG:Debug>>:-fno-omit-frame-pointer>
)
target_link_options(dodplace_flags INTERFACE
    $<$<AND:$<BOOL:${DODPLACE_SANITIZE}>,$<CONFIG:Debug>>:-fsanitize=address,undefined>
)

# --- Library ----------------------------------------------------------------
add_library(place STATIC
    src/arena.c
    src/context.c
    src/context_courtyard.c
    src/context_validate.c
    src/context_validate_board.c
    src/context_validate_rules.c
    src/context_validate_arrays.c
    src/io.c
    src/scene_load.c
    src/io_sections.c
    src/io_table.c
    src/placement_io.c
    src/scene_save.c
    src/solver_core.c
    src/shape.c
    src/copper_test.c
    src/ratsnest.c
    src/crossings.c
    src/dct.c
    src/density_grid.c
    src/density_solve.c
    src/global_forces.c
    src/solver_union.c
    src/solver_state.c
    src/solver_bbox.c
    src/solver_params.c
    src/pin_torque.c
    src/solver_limits.c
    src/solver_geometry.c
    src/spatial_grid.c
    src/spatial_query.c
    src/cost_metrics.c
    src/overlap_collect.c
    src/overlap_count.c
    src/solver_cluster.c
    src/solver_best.c
    src/solver_global.c
    src/solver_analytic.c
    src/solver_restarts.c
    src/solver_refine.c
    src/solver_legal.c
    src/legalize_keepout.c
    src/polish.c
    src/swap.c
    src/swap_assign.c
    src/swap_buckets.c
    src/swap_detailed.c
    src/swap_window.c
    src/assign_hungarian.c
    src/wa_wirelength.c
    src/lns_repair.c
    src/raster_mask.c
    src/matching_plan.c
    src/matching_slots.c
    src/matching_auction.c
)
add_library(dodplace::place ALIAS place)
target_include_directories(place PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(place PRIVATE dodplace_flags)

# --- Tools ------------------------------------------------------------------
add_executable(dodplace-solve src/main.c)
target_link_libraries(dodplace-solve PRIVATE place dodplace_flags)

# --- Tests ------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)

# --- Python front end (optional) --------------------------------------------
# The Python suite is normally run with `uv run pytest` from python/. Wire it
# into ctest too when asked, so a single command covers the whole project.
# --- Threads ----------------------------------------------------------------
# The multi-start runs one annealing walk per core, each with its own arena, so
# the build has to know where pthreads live (nothing to link on macOS, -pthread
# elsewhere).
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
target_link_libraries(place PUBLIC Threads::Threads)

option(DODPLACE_PYTHON_TESTS "Run the python/ pytest suite from ctest" OFF)
if(DODPLACE_PYTHON_TESTS)
    find_program(UV_EXECUTABLE uv)
    if(NOT UV_EXECUTABLE)
        message(FATAL_ERROR "DODPLACE_PYTHON_TESTS=ON needs uv on PATH")
    endif()
    add_test(NAME python
        COMMAND ${UV_EXECUTABLE} run --extra dev pytest
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/python)
endif()
