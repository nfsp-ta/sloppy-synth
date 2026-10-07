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

add_executable(control_tests control_tests.cpp)
target_link_libraries(control_tests PRIVATE sloppy_engine ${CMAKE_DL_LIBS})
# Exported symbols let the crash reporter name functions in CI logs.
set_target_properties(control_tests PROPERTIES ENABLE_EXPORTS ON)
target_compile_definitions(control_tests PRIVATE
  SLOPPY_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
  SLOPPY_WEB_DIR="${PROJECT_SOURCE_DIR}/web")
add_test(NAME control_tests COMMAND control_tests)
# A hung socket test should fail in minutes, not at ctest's 25-minute default.
set_tests_properties(control_tests PROPERTIES TIMEOUT 300)

# The Android app's MIDI parser is plain C++, so it's tested here too.
set(ANDROID_CPP_DIR ${PROJECT_SOURCE_DIR}/android/app/src/main/cpp)
add_executable(midi_parser_tests midi_parser_tests.cpp ${ANDROID_CPP_DIR}/midi_parser.cpp)
target_include_directories(midi_parser_tests PRIVATE ${ANDROID_CPP_DIR})
add_test(NAME midi_parser_tests COMMAND midi_parser_tests)
