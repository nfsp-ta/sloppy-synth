add_executable(engine_tests engine_tests.cpp)
target_link_libraries(engine_tests PRIVATE sloppy_engine)
target_compile_definitions(engine_tests PRIVATE
  SLOPPY_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

# CMAKE_CROSSCOMPILING_EMULATOR (e.g. qemu-aarch64) is applied automatically,
# so the same tests run on cross builds.
add_test(NAME engine_tests COMMAND engine_tests)

add_test(NAME render_init_patch
         COMMAND sloppy-render -o ${CMAKE_CURRENT_BINARY_DIR}/init.wav -n C3,E3,G3 -l 1 -t 0.5)
add_test(NAME render_test_bass
         COMMAND sloppy-render ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/test_bass.vital
                 -o ${CMAKE_CURRENT_BINARY_DIR}/test_bass.wav -n C2 -l 1 -t 0.5)
add_test(NAME render_rejects_future_patch
         COMMAND sloppy-render ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/from_the_future.vital
                 -o ${CMAKE_CURRENT_BINARY_DIR}/future.wav)
set_tests_properties(render_rejects_future_patch PROPERTIES WILL_FAIL TRUE)
