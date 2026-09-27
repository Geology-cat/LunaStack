// 入力の前処理（デバイヤー方式・ダーク/フラット補正・フレーム範囲・色形式の指定・
// 静止画連番）と、メタデータ・品質キャッシュのテスト。

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "microtest.hpp"
#include "stackcore/calibration.hpp"
#include "stackcore/debayer.hpp"
#include "stackcore/fits_writer.hpp"
#include "stackcore/global_stage.hpp"
#include "stackcore/image_reader.hpp"
#include "stackcore/map_pipeline.hpp"
#include "stackcore/metadata.hpp"
#include "stackcore/png_writer.hpp"
#include "stackcore/sidecar.hpp"
#include "stackcore/tiff_writer.hpp"
#include "stackcore/video_source.hpp"
#include "synthetic_ser.hpp"

using stackcore::FrameBuffer;
using stackcore::OpenOptions;
using stackcore::SerColorId;

namespace {

std::string temp_path(const std::string& name) { return "/tmp/lunastack_test_" + name; }

// 実写に近い色画像。輝度の構造（縞と細部）を全チャンネルが共有し、色は緩やかに変わる。
// MHCは「色の間で輪郭がそろっている」ことを使う方式なので、実画像と同じくそうしておく。
FrameBuffer smooth_color(int w, int h) {
    FrameBuffer f(w, h, 3);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const double l = 0.45 + 0.25 * std::sin(x * 0.55) * std::cos(y * 0.35) +
                             0.1 * std::sin((x + y) * 0.9);
            f.row(0, y)[x] = static_cast<float>(l * (0.9 + 0.05 * std::sin(y * 0.05)));
            f.row(1, y)[x] = static_cast<float>(l);
            f.row(2, y)[x] = static_cast<float>(l * (0.7 + 0.05 * std::cos(x * 0.04)));
        }
    }
    return f;
}

// RGGBでモザイク化する。
FrameBuffer mosaic_rggb(const FrameBuffer& rgb) {
    FrameBuffer cfa(rgb.width(), rgb.height(), 1);
    for (int y = 0; y < rgb.height(); ++y) {
        for (int x = 0; x < rgb.width(); ++x) {
            const int c = (y % 2 == 0) ? (x % 2 == 0 ? 0 : 1) : (x % 2 == 0 ? 1 : 2);
            cfa.row(0, y)[x] = rgb.row(c, y)[x];
        }
    }
    return cfa;
}

double rms_interior(const FrameBuffer& a, const FrameBuffer& b, int border) {
    double sum = 0.0;
    std::size_t n = 0;
    for (int c = 0; c < a.channels(); ++c) {
        for (int y = border; y < a.height() - border; ++y) {
            for (int x = border; x < a.width() - border; ++x) {
                const double d = a.row(c, y)[x] - b.row(c, y)[x];
                sum += d * d;
                ++n;
            }
        }
    }
    return std::sqrt(sum / n);
}

void write_pattern_png(const std::string& path, int w, int h, int frame) {
    FrameBuffer f(w, h, 1);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) f.row(0, y)[x] = ((x + 3 * y + 17 * frame) % 50) / 49.0f;
    }
    stackcore::write_png16(path, f);
}

std::string make_dir(const std::string& name) {
    const std::string dir = temp_path(name);
    std::system(("rm -rf '" + dir + "' && mkdir -p '" + dir + "'").c_str());
    return dir;
}

}  // namespace

// ---- デバイヤー --------------------------------------------------------------

MT_TEST(debayer_MHCは一様な色を正確に戻す) {
    FrameBuffer rgb(16, 12, 3);
    for (int y = 0; y < 12; ++y) {
        for (int x = 0; x < 16; ++x) {
            rgb.row(0, y)[x] = 0.2f;
            rgb.row(1, y)[x] = 0.5f;
            rgb.row(2, y)[x] = 0.8f;
        }
    }
    FrameBuffer out;
    stackcore::debayer_mhc(mosaic_rggb(rgb), SerColorId::BayerRGGB, out);
    for (int c = 0; c < 3; ++c) {
        for (int y = 0; y < 12; ++y) {
            for (int x = 0; x < 16; ++x) MT_CHECK_NEAR(out.row(c, y)[x], rgb.row(c, y)[x], 1e-6);
        }
    }
}

MT_TEST(debayer_MHCは滑らかな画像でbilinearより誤差が小さい) {
    const FrameBuffer rgb = smooth_color(64, 48);
    const FrameBuffer cfa = mosaic_rggb(rgb);
    FrameBuffer bilinear, mhc;
    stackcore::debayer(cfa, SerColorId::BayerRGGB, stackcore::DebayerMethod::Bilinear, bilinear);
    stackcore::debayer(cfa, SerColorId::BayerRGGB, stackcore::DebayerMethod::MalvarHeCutler, mhc);
    const double e_bilinear = rms_interior(bilinear, rgb, 3);
    const double e_mhc = rms_interior(mhc, rgb, 3);
    std::printf("           bilinear RMS %.5f / MHC RMS %.5f\n", e_bilinear, e_mhc);
    MT_CHECK(e_mhc < e_bilinear * 0.7);
}

// ---- キャリブレーション ------------------------------------------------------

MT_TEST(calibration_ダークを引きフラットで割る) {
    FrameBuffer frame(4, 4, 1), dark(4, 4, 1), master_flat(4, 4, 1);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            dark.row(0, y)[x] = 0.05f;
            // 左半分は感度が半分（周辺減光やホコリの影を模す）。
            master_flat.row(0, y)[x] = x < 2 ? 0.25f : 0.5f;
            frame.row(0, y)[x] = 0.05f + (x < 2 ? 0.2f : 0.4f);  // 真の明るさはどこも0.4相当
        }
    }
    stackcore::CalibrationFrames cal;
    cal.dark = std::move(dark);
    cal.flat = stackcore::normalize_flat(master_flat, SerColorId::Mono);
    stackcore::apply_calibration(frame, cal);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) MT_CHECK_NEAR(frame.row(0, y)[x], frame.row(0, 0)[0], 1e-6);
    }
    MT_CHECK_NEAR(frame.row(0, 0)[0], 0.3, 1e-5);  // フラットは平均0.375で正規化される

    // 寸法が合わなければ例外
    FrameBuffer wrong(3, 4, 1);
    MT_CHECK_THROWS(stackcore::apply_calibration(wrong, cal));
}

MT_TEST(calibration_Bayerのフラットは位相ごとに正規化する) {
    FrameBuffer flat(4, 4, 1);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            // R位置0.8、G位置0.4、B位置0.2（色ごとの感度差。これは補正してはいけない）
            const int c = (y % 2 == 0) ? (x % 2 == 0 ? 0 : 1) : (x % 2 == 0 ? 1 : 2);
            flat.row(0, y)[x] = c == 0 ? 0.8f : (c == 1 ? 0.4f : 0.2f);
        }
    }
    const FrameBuffer n = stackcore::normalize_flat(flat, SerColorId::BayerRGGB);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) MT_CHECK_NEAR(n.row(0, y)[x], 1.0, 1e-6);
    }
}

MT_TEST(calibration_マスターは全フレームの平均になる) {
    synthetic::SerSpec spec;
    spec.width = 8;
    spec.height = 6;
    spec.frames = 4;
    spec.pixel_depth = 16;
    spec.with_noise = false;
    const std::string path = temp_path("master.ser");
    synthetic::write_ser(path, spec);
    const auto source = stackcore::open_video(path, OpenOptions());
    const FrameBuffer master = stackcore::build_master_frame(*source);
    FrameBuffer f;
    double expected = 0.0;
    for (int i = 0; i < 4; ++i) {
        source->read_frame(i, f);
        expected += f.row(0, 2)[3];
    }
    MT_CHECK_NEAR(master.row(0, 2)[3], expected / 4.0, 1e-6);
    std::remove(path.c_str());
}

// ---- 前処理ラッパー ----------------------------------------------------------

MT_TEST(prepared_既定値なら元のソースと同じ結果になる) {
    synthetic::SerSpec spec;
    spec.width = 48;
    spec.height = 40;
    spec.frames = 12;
    spec.color_id = 8;  // RGGB
    const std::string path = temp_path("prepared_default.ser");
    synthetic::write_ser(path, spec);

    OpenOptions plain;
    MT_CHECK(!plain.has_preprocessing());
    const auto a = stackcore::open_video(path, plain);
    const auto b = stackcore::open_raw_video(path, plain);
    MT_CHECK_EQ(a->describe(), b->describe());

    // 全範囲を明示して包んでも、スタック結果はバイト単位で一致する。
    OpenOptions full;
    full.frame_start = 0;
    full.frame_end = 12;
    full.override_color = true;
    full.color_override = SerColorId::BayerRGGB;
    MT_CHECK(full.has_preprocessing());
    const auto wrapped = stackcore::open_video(path, full);
    stackcore::GlobalStageSettings settings;
    const auto report_a = stackcore::run_global_stage(*a, settings, false, stackcore::ProgressFn());
    const auto report_w = stackcore::run_global_stage(*wrapped, settings, false, stackcore::ProgressFn());
    const auto sel_a = stackcore::select_top_frames(report_a.frames, 50.0);
    const auto sel_w = stackcore::select_top_frames(report_w.frames, 50.0);
    const FrameBuffer sa = stackcore::build_global_reference(*a, report_a, sel_a, false, stackcore::ProgressFn());
    const FrameBuffer sw = stackcore::build_global_reference(*wrapped, report_w, sel_w, false, stackcore::ProgressFn());
    MT_CHECK_EQ(sa.channels(), 3);
    for (int c = 0; c < 3; ++c) {
        for (int y = 0; y < sa.height(); ++y) {
            MT_CHECK(std::memcmp(sa.row(c, y), sw.row(c, y), sizeof(float) * sa.width()) == 0);
        }
    }
    std::remove(path.c_str());
}

MT_TEST(prepared_フレーム範囲と元の番号とタイムスタンプ) {
    synthetic::SerSpec spec;
    spec.frames = 10;
    const std::string path = temp_path("prepared_range.ser");
    synthetic::write_ser(path, spec);
    OpenOptions o;
    o.frame_start = 3;
    o.frame_end = 7;
    const auto src = stackcore::open_video(path, o);
    const auto raw = stackcore::open_raw_video(path, OpenOptions());
    MT_CHECK_EQ(src->frame_count(), 4);
    MT_CHECK_EQ(src->original_index(0), 3);
    MT_CHECK_EQ(src->original_index(3), 6);
    MT_CHECK_EQ(src->timestamp_ticks(1), raw->timestamp_ticks(4));
    FrameBuffer a, b;
    src->read_frame(2, a);
    raw->read_frame(5, b);
    MT_CHECK(std::memcmp(a.row(0, 3), b.row(0, 3), sizeof(float) * a.width()) == 0);
    MT_CHECK_THROWS(src->read_frame(4, a));

    OpenOptions empty;
    empty.frame_start = 8;
    empty.frame_end = 8;
    MT_CHECK_THROWS(stackcore::open_video(path, empty));
    std::remove(path.c_str());
}

MT_TEST(prepared_色形式の指定とデバイヤー方式が読み込みに効く) {
    synthetic::SerSpec spec;
    spec.width = 16;
    spec.height = 12;
    spec.frames = 2;
    spec.color_id = 0;  // ヘッダはモノクロ
    const std::string path = temp_path("prepared_color.ser");
    synthetic::write_ser(path, spec);
    OpenOptions o;
    o.override_color = true;
    o.color_override = SerColorId::BayerGRBG;
    o.debayer = stackcore::DebayerMethod::MalvarHeCutler;
    const auto src = stackcore::open_video(path, o);
    MT_CHECK(src->color_id() == SerColorId::BayerGRBG);
    MT_CHECK(src->debayer_method() == stackcore::DebayerMethod::MalvarHeCutler);
    FrameBuffer cfa, rgb;
    const FrameBuffer* prepared = stackcore::read_prepared_frame(*src, 0, false, cfa, rgb);
    MT_CHECK_EQ(prepared->channels(), 3);
    FrameBuffer expected;
    stackcore::debayer_mhc(cfa, SerColorId::BayerGRBG, expected);
    MT_CHECK(std::memcmp(prepared->row(1, 5), expected.row(1, 5), sizeof(float) * 16) == 0);

    OpenOptions bad;
    bad.override_color = true;
    bad.color_override = SerColorId::BayerCYYM;
    MT_CHECK_THROWS(stackcore::open_video(path, bad));
    std::remove(path.c_str());
}

MT_TEST(prepared_ダーク補正がすべての読み込みに掛かる) {
    synthetic::SerSpec spec;
    spec.width = 8;
    spec.height = 6;
    spec.frames = 3;
    spec.with_noise = false;
    const std::string path = temp_path("prepared_dark.ser");
    synthetic::write_ser(path, spec);
    auto cal = std::make_shared<stackcore::CalibrationFrames>();
    cal->dark.reset(8, 6, 1);
    for (int y = 0; y < 6; ++y) {
        for (int x = 0; x < 8; ++x) cal->dark.row(0, y)[x] = 0.1f;
    }
    OpenOptions o;
    o.calibration = cal;
    const auto src = stackcore::open_video(path, o);
    const auto raw = stackcore::open_raw_video(path, OpenOptions());
    FrameBuffer a, b;
    src->read_frame(1, a);
    raw->read_frame(1, b);
    MT_CHECK_NEAR(a.row(0, 2)[5], std::max(0.0f, b.row(0, 2)[5] - 0.1f), 1e-6);

    auto wrong = std::make_shared<stackcore::CalibrationFrames>();
    wrong->dark.reset(7, 6, 1);
    OpenOptions ow;
    ow.calibration = wrong;
    MT_CHECK_THROWS(stackcore::open_video(path, ow));
    std::remove(path.c_str());
}

// ---- 静止画連番 --------------------------------------------------------------

MT_TEST(sequence_フォルダの静止画を自然順に1本の動画として読む) {
    const std::string dir = make_dir("sequence");
    // わざと辞書順と自然順が食い違う名前にする。
    const int order[] = {1, 2, 10};
    for (int i = 0; i < 3; ++i) {
        write_pattern_png(dir + "/frame" + std::to_string(order[i]) + ".png", 20, 14, order[i]);
    }
    std::FILE* fp = std::fopen((dir + "/notes.txt").c_str(), "wb");  // 画像以外は無視する
    std::fclose(fp);

    const auto src = stackcore::open_video(dir, OpenOptions());
    MT_CHECK_EQ(src->frame_count(), 3);
    MT_CHECK_EQ(src->width(), 20);
    MT_CHECK_EQ(std::string(src->format_name()), std::string("静止画連番"));
    FrameBuffer f;
    src->read_frame(2, f);  // 3番目は frame10
    MT_CHECK_NEAR(f.row(0, 1)[4], ((4 + 3 + 170) % 50) / 49.0, 1.0 / 65535.0);

    // 明示リストでも開ける。寸法の違う画像が混ざれば読んだ時点で例外。
    write_pattern_png(dir + "/odd.png", 21, 14, 0);
    OpenOptions o;
    o.sequence_files = {dir + "/frame1.png", dir + "/odd.png"};
    const auto mixed = stackcore::open_video("", o);
    MT_CHECK_EQ(mixed->frame_count(), 2);
    MT_CHECK_THROWS(mixed->read_frame(1, f));

    // 静止画連番でも品質評価からスタックまで通る。
    const auto seq = stackcore::open_video(dir + "/", OpenOptions());
    std::system(("rm -f '" + dir + "/odd.png'").c_str());
    const auto seq3 = stackcore::open_video(dir, OpenOptions());
    stackcore::GlobalStageSettings settings;
    const auto report = stackcore::run_global_stage(*seq3, settings, false, stackcore::ProgressFn());
    MT_CHECK_EQ(static_cast<int>(report.frames.size()), 3);
    (void)seq;
    std::system(("rm -rf '" + dir + "'").c_str());
}

// ---- メタデータ --------------------------------------------------------------

MT_TEST(metadata_タイムスタンプを暦とWinJUPOS形式に直す) {
    // 2021-07-08 10:50:27.300 UTC（Python の datetime で求めた .NET ticks）
    const std::int64_t ticks = 637613382273000000LL;
    MT_CHECK_EQ(stackcore::ticks_to_iso8601(ticks), std::string("2021-07-08T10:50:27.300"));
    MT_CHECK_EQ(stackcore::ticks_to_tiff_datetime(ticks), std::string("2021:07:08 10:50:27"));
    // 27.3秒 → 分の10分の1（6秒単位）に四捨五入して 50.5分
    MT_CHECK_EQ(stackcore::ticks_to_winjupos(ticks), std::string("2021-07-08-1050_5"));
    // 閏年の2月29日
    MT_CHECK_EQ(stackcore::ticks_to_iso8601(630873792000000000LL).substr(0, 10),
                std::string("2000-02-29"));
}

MT_TEST(metadata_空なら従来と同じバイト列で_付ければ読み戻せる) {
    FrameBuffer img(9, 7, 3);
    for (int c = 0; c < 3; ++c) {
        for (int y = 0; y < 7; ++y) {
            for (int x = 0; x < 9; ++x) img.row(c, y)[x] = (x + y + c) / 20.0f;
        }
    }
    const auto read_bytes = [](const std::string& p) {
        std::vector<unsigned char> v;
        std::FILE* fp = std::fopen(p.c_str(), "rb");
        int ch;
        while ((ch = std::fgetc(fp)) != EOF) v.push_back(static_cast<unsigned char>(ch));
        std::fclose(fp);
        return v;
    };
    const std::string a = temp_path("meta_a.tif"), b = temp_path("meta_b.tif");
    stackcore::write_tiff(a, img, stackcore::TiffFormat::UInt16);
    stackcore::write_tiff(b, img, stackcore::TiffFormat::UInt16, stackcore::ImageMetadata());
    MT_CHECK(read_bytes(a) == read_bytes(b));

    stackcore::ImageMetadata meta;
    meta.software = "LunaStack test";
    meta.description = "テスト：処理条件";
    meta.date_obs = "2021-07-08T10:50:27.300";
    meta.frames_combined = 25;
    meta.history.push_back("stack mean 25 frames");
    const std::string t = temp_path("meta.tif"), p = temp_path("meta.png"), f = temp_path("meta.fits");
    stackcore::write_tiff(t, img, stackcore::TiffFormat::UInt16, meta);
    stackcore::write_png16(p, img, meta);
    stackcore::write_fits_float32(f, img, meta);
    for (const std::string& path : {t, p, f}) {
        FrameBuffer back;
        stackcore::ImageFileInfo info;
        stackcore::read_image_file(path, back, info);
        MT_CHECK_EQ(back.width(), 9);
        MT_CHECK_NEAR(back.row(2, 6)[8], img.row(2, 6)[8], 1.0 / 65535.0);
    }
    const std::vector<unsigned char> fits = read_bytes(f);
    const std::string header(fits.begin(), fits.begin() + 2880);
    MT_CHECK(header.find("DATE-OBS= '2021-07-08T10:50:27.300'") != std::string::npos);
    MT_CHECK(header.find("NCOMBINE=") != std::string::npos);
    MT_CHECK(header.find("ROWORDER= 'TOP-DOWN'") != std::string::npos);
    MT_CHECK(fits.size() % 2880 == 0);
    const std::vector<unsigned char> tif = read_bytes(t);
    const std::string tif_text(tif.begin(), tif.end());
    MT_CHECK(tif_text.find("2021:07:08 10:50:27") != std::string::npos);
    MT_CHECK(tif_text.find("LunaStack test") != std::string::npos);
    for (const std::string& path : {a, b, t, p, f}) std::remove(path.c_str());
}

// ---- 品質キャッシュ ----------------------------------------------------------

MT_TEST(quality_cache_保存して読み直せる) {
    stackcore::QualityCache c;
    c.source_size = 123456789;
    c.source_frames = 3;
    c.width = 640;
    c.height = 480;
    c.channels = 3;
    for (int i = 0; i < 3; ++i) {
        stackcore::FrameInfo f;
        f.index = i;
        f.quality = 0.1 * (i + 1);
        f.mean = 0.3 + 0.01 * i;
        c.report.frames.push_back(f);
    }
    c.report.reference_index = 1;
    c.report.reference_mean = 0.31;
    const std::string path = temp_path("quality.lstkq");
    stackcore::save_quality_cache(path, c);
    stackcore::QualityCache d;
    stackcore::load_quality_cache(path, d);
    MT_CHECK_EQ(d.source_size, c.source_size);
    MT_CHECK_EQ(d.channels, 3);
    MT_CHECK_EQ(static_cast<int>(d.report.frames.size()), 3);
    MT_CHECK_EQ(d.report.frames[2].quality, c.report.frames[2].quality);
    MT_CHECK_EQ(d.report.reference_index, 1);
    MT_CHECK(d.report.frames[0].accepted);
    std::remove(path.c_str());
    MT_CHECK_THROWS(stackcore::load_quality_cache(path, d));
}

// ---- 処理範囲 ----------------------------------------------------------------

MT_TEST(処理範囲_補正のあと切り出し左上と大きさを偶数にそろえる) {
    const std::string dir = make_dir("roi_seq");
    for (int i = 0; i < 3; ++i) {
        char name[64];
        std::snprintf(name, sizeof(name), "/f%02d.png", i);
        write_pattern_png(dir + name, 40, 30, i);
    }
    stackcore::OpenOptions full;
    std::unique_ptr<stackcore::VideoSource> whole = stackcore::open_video(dir, full);
    stackcore::OpenOptions o;
    o.roi_x = 5;  // 奇数 → 4 に切り下げ
    o.roi_y = 3;  // → 2
    o.roi_width = 13;   // 右端 18 → 幅 14
    o.roi_height = 9;   // 下端 12 → 高さ 10
    int x, y, w, h;
    stackcore::effective_roi(o, 40, 30, x, y, w, h);
    MT_CHECK_EQ(x, 4);
    MT_CHECK_EQ(y, 2);
    MT_CHECK_EQ(w, 14);
    MT_CHECK_EQ(h, 10);
    std::unique_ptr<stackcore::VideoSource> roi = stackcore::open_video(dir, o);
    MT_CHECK_EQ(roi->width(), 14);
    MT_CHECK_EQ(roi->height(), 10);
    MT_CHECK_EQ(roi->frame_count(), 3);
    FrameBuffer a, b;
    whole->read_frame(2, a);
    roi->read_frame(2, b);
    for (int yy = 0; yy < 10; ++yy) {
        for (int xx = 0; xx < 14; ++xx) MT_CHECK_EQ(b.row(0, yy)[xx], a.row(0, 2 + yy)[4 + xx]);
    }
    MT_CHECK(roi->describe().find("処理範囲 14×10") != std::string::npos);
    // 範囲が画像からはみ出す指定は収める。
    o.roi_x = 36;
    o.roi_width = 100;
    stackcore::effective_roi(o, 40, 30, x, y, w, h);
    MT_CHECK_EQ(x, 36);
    MT_CHECK_EQ(w, 4);
}

MT_TEST(処理範囲_ダークは全体に引いてから切り出す) {
    const std::string dir = make_dir("roi_dark");
    for (int i = 0; i < 2; ++i) {
        char name[64];
        std::snprintf(name, sizeof(name), "/f%02d.png", i);
        write_pattern_png(dir + name, 20, 16, i);
    }
    auto cal = std::make_shared<stackcore::CalibrationFrames>();
    cal->dark.reset(20, 16, 1);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 20; ++x) cal->dark.row(0, y)[x] = 0.01f * ((x + y) % 3);
    }
    stackcore::OpenOptions whole_o;
    whole_o.calibration = cal;
    stackcore::OpenOptions roi_o = whole_o;
    roi_o.roi_x = 6;
    roi_o.roi_y = 4;
    roi_o.roi_width = 8;
    roi_o.roi_height = 6;
    FrameBuffer a, b;
    stackcore::open_video(dir, whole_o)->read_frame(1, a);
    stackcore::open_video(dir, roi_o)->read_frame(1, b);
    for (int y = 0; y < 6; ++y) {
        for (int x = 0; x < 8; ++x) MT_CHECK_EQ(b.row(0, y)[x], a.row(0, 4 + y)[6 + x]);
    }
}
