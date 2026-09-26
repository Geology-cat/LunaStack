# LibRaw 0.22.2（無改変）を静的ライブラリとしてビルドする。
#
# 使うのは RAW の展開（unpack）と、機種ごとの切り抜き・黒・白の情報だけ。
# 色補間・色変換・ガンマ（dcraw_process）は使わない（仕様書 §3.1）。
# ライセンスは LGPL 2.1 と CDDL 1.0 の選択制で、LunaStack は **CDDL 1.0** を選ぶ
# （third_party/LibRaw/README.LunaStack.md）。
#
# 最適化は本体と同じ -O2・-fno-fast-math（決定論性、仕様書 §7.1）。
# 10.13 より新しいAPIを使えばコンパイルエラーになるよう、availability の検査は本体と同じく掛ける。
# LibRaw 自身の警告は多いので -w で黙らせる（コードは直さない）。
add_library(libraw_static STATIC
    src/libraw_datastream.cpp
    src/libraw_c_api.cpp
    src/tables/cameralist.cpp
    src/decoders/fuji_compressed.cpp
    src/decoders/crx.cpp
    src/decoders/pana8.cpp
    src/decoders/fp_dng.cpp
    src/decoders/decoders_libraw.cpp
    src/decoders/olympus14.cpp
    src/decoders/sonycc.cpp
    src/decompressors/losslessjpeg.cpp
    src/decoders/unpack.cpp
    src/decoders/unpack_thumb.cpp
    src/integration/rawspeed_glue.cpp
    src/integration/dngsdk_glue.cpp
    src/tables/colorconst.cpp
    src/utils/utils_libraw.cpp
    src/utils/init_close_utils.cpp
    src/utils/decoder_info.cpp
    src/utils/open.cpp
    src/utils/phaseone_processing.cpp
    src/utils/thumb_utils.cpp
    src/write/tiff_writer.cpp
    src/preprocessing/subtract_black.cpp
    src/postprocessing/postprocessing_utils.cpp
    src/postprocessing/dcraw_process.cpp
    src/preprocessing/raw2image.cpp
    src/postprocessing/mem_image.cpp
    src/x3f/x3f_utils_patched.cpp
    src/x3f/x3f_parse_process.cpp
    src/utils/read_utils.cpp
    src/utils/curves.cpp
    src/utils/utils_dcraw.cpp
    src/tables/colordata.cpp
    src/decoders/canon_600.cpp
    src/decoders/decoders_dcraw.cpp
    src/decoders/decoders_libraw_dcrdefs.cpp
    src/decoders/generic.cpp
    src/decoders/kodak_decoders.cpp
    src/decoders/dng.cpp
    src/decoders/smal.cpp
    src/decoders/load_mfbacks.cpp
    src/metadata/sony.cpp
    src/metadata/nikon.cpp
    src/metadata/samsung.cpp
    src/metadata/cr3_parser.cpp
    src/metadata/canon.cpp
    src/metadata/epson.cpp
    src/metadata/olympus.cpp
    src/metadata/leica.cpp
    src/metadata/fuji.cpp
    src/metadata/adobepano.cpp
    src/metadata/pentax.cpp
    src/metadata/p1.cpp
    src/metadata/makernotes.cpp
    src/metadata/exif_gps.cpp
    src/metadata/kodak.cpp
    src/metadata/tiff.cpp
    src/metadata/ciff.cpp
    src/metadata/mediumformat.cpp
    src/metadata/minolta.cpp
    src/metadata/identify_tools.cpp
    src/metadata/hasselblad_model.cpp
    src/metadata/normalize_model.cpp
    src/metadata/identify.cpp
    src/metadata/misc_parsers.cpp
    src/tables/wblists.cpp
    src/postprocessing/postprocessing_aux.cpp
    src/postprocessing/postprocessing_utils_dcrdefs.cpp
    src/postprocessing/aspect_ratio.cpp
    src/demosaic/misc_demosaic.cpp
    src/demosaic/xtrans_demosaic.cpp
    src/demosaic/ahd_demosaic.cpp
    src/demosaic/dht_demosaic.cpp
    src/demosaic/aahd_demosaic.cpp
    src/demosaic/dcb_demosaic.cpp
    src/write/file_write.cpp
    src/preprocessing/ext_preprocess.cpp
    src/write/apply_profile.cpp
)

target_include_directories(libraw_static PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# LIBRAW_NOTHREADS を付けない＝スレッド安全版（libraw_r 相当）。
# 1回の読み込みごとに LibRaw オブジェクトを作るので、並列に読んでもよい。
target_compile_options(libraw_static PRIVATE
    -O2
    -fno-fast-math
    -w
    -Werror=unguarded-availability
    -Werror=unguarded-availability-new
)
set_target_properties(libraw_static PROPERTIES CXX_STANDARD 17 CXX_EXTENSIONS OFF)
