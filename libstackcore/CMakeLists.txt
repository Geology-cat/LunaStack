add_library(stackcore STATIC
    src/image/frame_buffer.cpp
    src/image/debayer.cpp
    src/image/calibration.cpp
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
    src/io/inflate.cpp
    src/io/image_reader.cpp
    src/io/raw_reader.cpp
    src/io/libraw_reader.cpp
    src/io/metadata.cpp
    src/io/sidecar.cpp
    src/post/wavelet.cpp
    src/post/finishing.cpp
    src/pipeline/global_stage.cpp
    src/pipeline/map_pipeline.cpp
    src/io/tiff_writer.cpp
    src/io/png_writer.cpp
    src/io/fits_writer.cpp
)

target_include_directories(stackcore PUBLIC include)
target_link_libraries(stackcore PUBLIC lunastack_flags)
# CR2・DNG 以外のカメラRAW（CR3・NEF・ARW・RAF など）。third_party/LibRaw 参照。
target_link_libraries(stackcore PRIVATE libraw_static)

# vDSP（FFT・ベクトル演算）。macOS 10.0 から存在し Intel/ARM 双方で最適化される。
target_link_libraries(stackcore PUBLIC "-framework Accelerate")
