add_executable(stackcore_tests
    src/test_main.cpp
    src/test_frame_buffer.cpp
    src/test_ser.cpp
    src/test_tiff.cpp
    src/test_png.cpp
    src/test_debayer.cpp
    src/test_quality.cpp
    src/test_phase_correlate.cpp
    src/test_global_align.cpp
    src/test_stack.cpp
    src/test_frame_selector.cpp
    src/test_avi.cpp
    src/test_resample.cpp
    src/test_zncc.cpp
    src/test_ap_placer.cpp
    src/test_windowed_stack.cpp
    src/test_map_pipeline.cpp
    src/test_wavelet.cpp
    src/test_drizzle.cpp
)

target_include_directories(stackcore_tests PRIVATE include)
target_link_libraries(stackcore_tests PRIVATE stackcore)

add_test(NAME stackcore_tests COMMAND stackcore_tests)

# ---- availabilityガードが実際に発火することの証明 ------------------------
# このターゲットは「ビルドが失敗すること」が期待値。
# ビルドが通ってしまったらガードが効いていないということであり、
# 10.13対応の主張そのものが無検証になる（実装計画書 §5）。
add_executable(availability_violation EXCLUDE_FROM_ALL src/availability_violation.cpp)
target_link_libraries(availability_violation PRIVATE lunastack_flags)

add_test(NAME availability_guard_fires
         COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
                 --target availability_violation --config $<CONFIG>)
set_tests_properties(availability_guard_fires PROPERTIES WILL_FAIL TRUE)
