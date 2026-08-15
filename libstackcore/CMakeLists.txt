add_library(stackcore STATIC
    src/image/frame_buffer.cpp
    src/image/debayer.cpp
    src/image/quality.cpp
    src/image/resample.cpp
    src/registration/phase_correlate.cpp
    src/registration/global_aligner.cpp
    src/registration/zncc_matcher.cpp
    src/map/ap_placer.cpp
    src/map/local_aligner.cpp
    src/stack/windowed_stacker.cpp
    src/stack/drizzle.cpp
    src/stack/simple_stacker.cpp
    src/stack/frame_selector.cpp
    src/io/mapped_file.cpp
    src/io/ser_decoder.cpp
    src/io/avi_decoder.cpp
    src/io/jpeg_decoder.cpp
    src/io/video_source.cpp
    src/io/sidecar.cpp
    src/post/wavelet.cpp
    src/pipeline/global_stage.cpp
    src/pipeline/map_pipeline.cpp
    src/io/tiff_writer.cpp
    src/io/png_writer.cpp
)

target_include_directories(stackcore PUBLIC include)
target_link_libraries(stackcore PUBLIC lunastack_flags)

# vDSP（FFT・ベクトル演算）。macOS 10.0 から存在し Intel/ARM 双方で最適化される。
target_link_libraries(stackcore PUBLIC "-framework Accelerate")
