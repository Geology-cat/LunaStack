#include <cmath>
#include <cstdint>
#include <vector>

#include "microtest.hpp"
#include "stackcore/ap_placer.hpp"
#include "stackcore/frame_buffer.hpp"

using stackcore::AlignmentPoint;
using stackcore::ApPlacementSettings;
using stackcore::FrameBuffer;
using stackcore::place_alignment_points;

namespace {

struct Lcg {
    std::uint32_t s;
    explicit Lcg(std::uint32_t seed) : s(seed) {}
    float next() {
        s = s * 1664525u + 1013904223u;
        return static_cast<float>((s >> 8) & 0xFFFF) / 65535.0f;
    }
};

// 暗背景に模様つきの円盤。惑星の代用。
FrameBuffer planet(int n, double radius, bool textured = true) {
    FrameBuffer fb(n, n, 1);
    Lcg rng(7);
    const double c = n / 2.0;
    for (int y = 0; y < n; ++y) {
        float* row = fb.row(0, y);
        for (int x = 0; x < n; ++x) {
            const double dx = x - c, dy = y - c;
            const double r = std::sqrt(dx * dx + dy * dy);
            double v;
            if (r <= radius) {
                v = 0.6;
                if (textured) v += 0.2 * std::sin(dy * 0.5) + 0.1 * std::cos(dx * 0.7);
            } else {
                v = 0.01;  // 宇宙空間
            }
            v += 0.004 * (rng.next() - 0.5);
            row[x] = static_cast<float>(v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v));
        }
    }
    fb.invalidate_luma();
    return fb;
}

bool covers(const std::vector<AlignmentPoint>& pts, int cx, int cy) {
    for (std::size_t i = 0; i < pts.size(); ++i) {
        if (pts[i].cx == cx && pts[i].cy == cy) return true;
    }
    return false;
}

}  // namespace

MT_TEST(ap_配置間隔はAPサイズの半分になる) {
    // 50%オーバーラップは⑧の継ぎ目防止の前提条件であり、単なる密度調整ではない。
    const FrameBuffer ref = planet(256, 110.0);
    ApPlacementSettings s;
    s.ap_size = 64;
    s.gradient_ratio = 0.0;  // 全部通す
    s.level_ratio = 0.0;
    int used = 0;
    const std::vector<AlignmentPoint> pts = place_alignment_points(ref, s, used);

    MT_CHECK_EQ(used, 64);
    MT_CHECK(!pts.empty());

    // x座標の刻みが32であること
    int min_x = 1 << 30, second = 1 << 30;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        const int x = pts[i].cx;
        if (x < min_x) {
            second = min_x;
            min_x = x;
        } else if (x > min_x && x < second) {
            second = x;
        }
    }
    MT_CHECK_EQ(min_x, 32);       // 最初の中心は APサイズの半分
    MT_CHECK_EQ(second - min_x, 32);  // 刻みは APサイズの半分
}

MT_TEST(ap_宇宙空間にはAPを置かない) {
    const FrameBuffer ref = planet(256, 70.0);
    ApPlacementSettings s;
    s.ap_size = 48;
    int used = 0;
    const std::vector<AlignmentPoint> pts = place_alignment_points(ref, s, used);

    MT_CHECK(!pts.empty());
    const double c = 128.0;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        const double dx = pts[i].cx - c, dy = pts[i].cy - c;
        const double r = std::sqrt(dx * dx + dy * dy);
        // 円盤の外側にAP中心が出ていないこと（縁のAPは半径+半サイズ程度まで許す）
        if (r > 70.0 + 24.0) {
            microtest::fail("宇宙空間にAPが置かれた: (" + microtest::mt_str(pts[i].cx) + "," +
                            microtest::mt_str(pts[i].cy) + ") r=" + microtest::mt_str(r));
            return;
        }
    }
    // 四隅は必ず除外されている
    MT_CHECK(!covers(pts, 24, 24));
}

MT_TEST(ap_模様のない領域にはAPを置かない) {
    // 輝度は十分でも勾配がなければ相関が立たないので置いてはいけない。
    const FrameBuffer flat_disc = planet(256, 100.0, false);
    ApPlacementSettings s;
    s.ap_size = 64;
    int used = 0;
    const std::vector<AlignmentPoint> flat_pts = place_alignment_points(flat_disc, s, used);

    const FrameBuffer textured = planet(256, 100.0, true);
    const std::vector<AlignmentPoint> tex_pts = place_alignment_points(textured, s, used);

    if (!(flat_pts.size() < tex_pts.size())) {
        microtest::fail("模様の有無でAP数が変わらない: 平坦=" +
                        microtest::mt_str(flat_pts.size()) +
                        " 模様あり=" + microtest::mt_str(tex_pts.size()));
    }
}

MT_TEST(ap_APは画像からはみ出さない) {
    const FrameBuffer ref = planet(200, 90.0);
    ApPlacementSettings s;
    s.ap_size = 64;
    s.gradient_ratio = 0.0;
    s.level_ratio = 0.0;
    int used = 0;
    const std::vector<AlignmentPoint> pts = place_alignment_points(ref, s, used);

    for (std::size_t i = 0; i < pts.size(); ++i) {
        MT_CHECK(pts[i].cx - 32 >= 0);
        MT_CHECK(pts[i].cy - 32 >= 0);
        MT_CHECK(pts[i].cx + 32 <= 200);
        MT_CHECK(pts[i].cy + 32 <= 200);
    }
}

MT_TEST(ap_順序はy昇順x昇順で固定される) {
    // 決定論性の要件。AP順が変わるとスタックの加算順が変わる。
    const FrameBuffer ref = planet(256, 110.0);
    ApPlacementSettings s;
    s.ap_size = 64;
    int used = 0;
    const std::vector<AlignmentPoint> pts = place_alignment_points(ref, s, used);

    for (std::size_t i = 1; i < pts.size(); ++i) {
        const bool ok = pts[i].cy > pts[i - 1].cy ||
                        (pts[i].cy == pts[i - 1].cy && pts[i].cx > pts[i - 1].cx);
        if (!ok) {
            microtest::fail("APの順序が y昇順→x昇順 になっていない");
            return;
        }
    }
}

MT_TEST(ap_サイズ自動提案は仕様の選択肢から選ぶ) {
    const int kAllowed[] = {32, 48, 64, 96, 128, 200};
    for (double radius : {40.0, 80.0, 120.0}) {
        const FrameBuffer ref = planet(320, radius);
        const int s = stackcore::suggest_ap_size(ref);
        bool found = false;
        for (int a : kAllowed) {
            if (a == s) found = true;
        }
        if (!found) {
            microtest::fail("提案されたAPサイズが選択肢にない: " + microtest::mt_str(s));
            return;
        }
    }
}

MT_TEST(ap_対象が大きいほど提案サイズも大きい) {
    const int small = stackcore::suggest_ap_size(planet(400, 40.0));
    const int large = stackcore::suggest_ap_size(planet(400, 180.0));
    if (!(small <= large)) {
        microtest::fail("対象が大きいのに提案APサイズが小さい: " + microtest::mt_str(small) +
                        " vs " + microtest::mt_str(large));
    }
}

MT_TEST(ap_手動指定があれば自動配置の代わりに使う) {
    // UI設計書 §5.2 の「クリックでAP追加、Deleteで削除」を支える経路。
    const FrameBuffer ref = planet(256, 90.0);

    ApPlacementSettings settings;
    settings.ap_size = 32;
    int auto_size = 0;
    const std::vector<AlignmentPoint> automatic =
        place_alignment_points(ref, settings, auto_size);
    MT_CHECK(automatic.size() > 3);

    // わざと並びを崩して渡す。返りは y 昇順 → x 昇順に揃うこと。
    ApPlacementSettings manual = settings;
    manual.use_manual_points = true;
    manual.manual_points.resize(3);
    manual.manual_points[0].cx = 160;
    manual.manual_points[0].cy = 128;
    manual.manual_points[1].cx = 128;
    manual.manual_points[1].cy = 96;
    manual.manual_points[2].cx = 100;
    manual.manual_points[2].cy = 128;

    int used = 0;
    const std::vector<AlignmentPoint> got = place_alignment_points(ref, manual, used);
    MT_CHECK_EQ(used, 32);
    MT_CHECK_EQ(static_cast<int>(got.size()), 3);
    MT_CHECK_EQ(got[0].cx, 128);
    MT_CHECK_EQ(got[0].cy, 96);
    MT_CHECK_EQ(got[1].cx, 100);
    MT_CHECK_EQ(got[1].cy, 128);
    MT_CHECK_EQ(got[2].cx, 160);
    MT_CHECK_EQ(got[2].cy, 128);

    // 測定値は埋まっていること（表示・診断で使う）。
    MT_CHECK(got[0].mean_gradient > 0.0);
    MT_CHECK(got[0].mean_level > 0.0);
}

MT_TEST(ap_手動指定は閾値で落とさないがはみ出す点は落とす) {
    // 自動配置なら模様が乏しくて落ちる背景上の点でも、
    // 利用者が明示的に置いたなら残す。黙って消えるのがいちばん困る。
    const FrameBuffer ref = planet(256, 90.0);

    ApPlacementSettings settings;
    settings.ap_size = 32;
    settings.use_manual_points = true;
    settings.manual_points.resize(3);
    settings.manual_points[0].cx = 24;  // 宇宙空間（自動なら落ちる）
    settings.manual_points[0].cy = 24;
    settings.manual_points[1].cx = 4;   // 左端からはみ出す
    settings.manual_points[1].cy = 128;
    settings.manual_points[2].cx = 250; // 右端からはみ出す
    settings.manual_points[2].cy = 128;

    int used = 0;
    const std::vector<AlignmentPoint> got = place_alignment_points(ref, settings, used);
    MT_CHECK_EQ(static_cast<int>(got.size()), 1);
    MT_CHECK_EQ(got[0].cx, 24);
    MT_CHECK_EQ(got[0].cy, 24);
}

MT_TEST(ap_すべて消去したら自動配置に戻さない) {
    // 「すべて消去」と「自動配置」は別の状態である。
    // 空リストを自動配置の合図にすると、消したのに勝手に置き直され、
    // 利用者から見て操作が効いていないのと同じになる。
    const FrameBuffer ref = planet(256, 90.0);

    ApPlacementSettings settings;
    settings.ap_size = 32;
    settings.use_manual_points = true;  // manual_points は空のまま

    int used = 0;
    const std::vector<AlignmentPoint> got = place_alignment_points(ref, settings, used);
    MT_CHECK(got.empty());
}
