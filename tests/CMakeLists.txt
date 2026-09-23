add_executable(test_arena test_arena.c)
target_link_libraries(test_arena PRIVATE place dodplace_flags)
add_test(NAME arena COMMAND test_arena)

add_executable(test_ingestion_degraded test_ingestion_degraded.c)
target_link_libraries(test_ingestion_degraded PRIVATE place dodplace_flags)
add_test(NAME ingestion_degraded COMMAND test_ingestion_degraded)

add_executable(test_scene_io test_scene_io.c)
target_link_libraries(test_scene_io PRIVATE place dodplace_flags)
target_compile_definitions(test_scene_io PRIVATE
    DODPLACE_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}"
    DODPLACE_FIXTURE_PATH="${CMAKE_SOURCE_DIR}/ir/fixtures/scene_minimal.bin"
)
add_test(NAME scene_io COMMAND test_scene_io)

add_executable(test_solver test_solver.c)
target_link_libraries(test_solver PRIVATE place dodplace_flags)
target_include_directories(test_solver PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME solver COMMAND test_solver)

add_executable(test_analytic test_analytic.c)
target_link_libraries(test_analytic PRIVATE place dodplace_flags)
target_include_directories(test_analytic PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME analytic COMMAND test_analytic)

add_executable(test_swap_window test_swap_window.c)
target_link_libraries(test_swap_window PRIVATE place dodplace_flags)
target_include_directories(test_swap_window PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME swap_window COMMAND test_swap_window)

add_executable(test_assign test_assign.c)
target_link_libraries(test_assign PRIVATE place dodplace_flags)
target_include_directories(test_assign PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME assign COMMAND test_assign)
