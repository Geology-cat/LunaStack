#!/usr/bin/env python3
"""既知の局所歪み場を持つ合成動画（SER）を作る。

MAP局所アライメントが実際に効いているかを判定するための道具。
実データでMAPの効果が見えないとき、それが実装の問題なのか
素材にそもそも局所歪みが無いのかを切り分けられない。
歪みの量を自分で決めた動画なら切り分けられる。

設計の要点:

* シーンは**連続関数**（ガウス斑点の重ね合わせ）として定義し、
  歪ませた座標で直接標本化する。画像を作ってからリサンプラで歪ませると、
  そのリサンプラの誤差が測定に混ざる。
* 斑点を使うのは非周期にするため。三角関数の重ね合わせは周期的で、
  相関に多義性が出る（このプロジェクトで実際に2回踏んだ罠）。
* 変位場は低周波の正弦の重ね合わせ。フレームごとに位相を変える。
  シーイングによる歪みは空間的に滑らかなので、それに合わせている。
* 真値（歪みなしで標本化した画像）も同時に書き出す。

使い方:
    python3 tools/make_seeing_sequence.py out.ser --truth truth.tif
    python3 tools/make_seeing_sequence.py out.ser --local 0 --global-shift 3
        （局所歪みなし＝グローバル平行移動だけ。対照実験用）
"""

import argparse
import math
import struct
import sys

import numpy as np

HEADER_SIZE = 178
FILE_ID = b"LUCAM-RECORDER"
COLOR_MONO = 0


def build_scene(size, margin, seed):
    """ガウス斑点の位置・振幅を決める。連続関数の係数にあたる。"""
    rng = np.random.RandomState(seed)
    extent = size + 2 * margin
    n = int(extent * extent / 110)
    xs = rng.uniform(-margin, size + margin, n)
    ys = rng.uniform(-margin, size + margin, n)
    amp = rng.uniform(0.3, 1.0, n)
    return xs, ys, amp


def sample_scene(xs, ys, amp, sigma, gx, gy):
    """任意の座標（配列）でシーンを評価する。

    gx, gy は評価したい座標。歪みを入れる場合は歪ませた座標を渡す。
    """
    out = np.zeros_like(gx, dtype=np.float64)
    inv = 1.0 / (2.0 * sigma * sigma)
    cut = (5.0 * sigma) ** 2
    for cx, cy, a in zip(xs, ys, amp):
        dx = gx - cx
        dy = gy - cy
        r2 = dx * dx + dy * dy
        m = r2 < cut
        if not m.any():
            continue
        out[m] += a * np.exp(-r2[m] * inv)
    return out


def warp_field(size, amplitude, seed, waves=4):
    """滑らかな低周波の変位場を作る。

    波長を100〜250px程度に取る。APサイズ（数十px）より十分長いので、
    AP内ではほぼ一様な平行移動に見える。これはシーイングの実際に近い。
    """
    rng = np.random.RandomState(seed)
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float64)
    dx = np.zeros((size, size))
    dy = np.zeros((size, size))
    for _ in range(waves):
        for target in (0, 1):
            wavelength = rng.uniform(100.0, 250.0)
            angle = rng.uniform(0.0, 2.0 * math.pi)
            phase = rng.uniform(0.0, 2.0 * math.pi)
            k = 2.0 * math.pi / wavelength
            proj = xx * math.cos(angle) + yy * math.sin(angle)
            wave = np.sin(k * proj + phase)
            if target == 0:
                dx += wave
            else:
                dy += wave
    # 振幅を指定値に正規化する（最大変位が amplitude 程度になるように）
    peak = max(np.abs(dx).max(), np.abs(dy).max(), 1e-9)
    return dx * (amplitude / peak), dy * (amplitude / peak)


def dotnet_ticks_now():
    return int((0 + 62135596800 + 1625741400) * 10_000_000)


def write_header(f, width, height, frames, depth):
    def fixed(text, n):
        return text.encode("ascii", "replace")[:n].ljust(n, b"\x00")

    f.write(FILE_ID)
    f.write(struct.pack("<i", 0))
    f.write(struct.pack("<i", COLOR_MONO))
    f.write(struct.pack("<i", 1))
    f.write(struct.pack("<i", width))
    f.write(struct.pack("<i", height))
    f.write(struct.pack("<i", depth))
    f.write(struct.pack("<i", frames))
    f.write(fixed("synthetic", 40))
    f.write(fixed("seeing simulation", 40))
    f.write(fixed("none", 40))
    ticks = dotnet_ticks_now()
    f.write(struct.pack("<q", ticks))
    f.write(struct.pack("<q", ticks))


def write_tiff16(path, img):
    """真値の書き出し用の最小16bit TIFF（1ch）。"""
    h, w = img.shape
    data = np.clip(img, 0.0, 1.0)
    data = (data * 65535.0 + 0.5).astype("<u2").tobytes()
    entries = [
        (256, 3, 1, w),           # ImageWidth
        (257, 3, 1, h),           # ImageLength
        (258, 3, 1, 16),          # BitsPerSample
        (259, 3, 1, 1),           # Compression
        (262, 3, 1, 1),           # Photometric = BlackIsZero
        (273, 4, 1, 0),           # StripOffsets（後で埋める）
        (277, 3, 1, 1),           # SamplesPerPixel
        (278, 3, 1, h),           # RowsPerStrip
        (279, 4, 1, len(data)),   # StripByteCounts
        (339, 3, 1, 1),           # SampleFormat = unsigned
    ]
    ifd_offset = 8
    ifd_size = 2 + 12 * len(entries) + 4
    strip_offset = ifd_offset + ifd_size

    with open(path, "wb") as f:
        f.write(b"II*\x00")
        f.write(struct.pack("<I", ifd_offset))
        f.write(struct.pack("<H", len(entries)))
        for tag, typ, count, value in entries:
            if tag == 273:
                value = strip_offset
            f.write(struct.pack("<HHI", tag, typ, count))
            if typ == 3 and count == 1:
                f.write(struct.pack("<HH", value, 0))
            else:
                f.write(struct.pack("<I", value))
        f.write(struct.pack("<I", 0))
        f.write(data)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("output")
    ap.add_argument("--size", type=int, default=256)
    ap.add_argument("--frames", type=int, default=300)
    ap.add_argument("--local", type=float, default=3.0,
                    help="局所歪みの最大振幅（px）。0でグローバル平行移動のみ")
    ap.add_argument("--global-shift", type=float, default=2.0,
                    help="フレームごとのグローバル平行移動の最大量（px）")
    ap.add_argument("--noise", type=float, default=0.02)
    ap.add_argument("--sigma", type=float, default=1.6, help="斑点の広がり（px）")
    ap.add_argument("--seed", type=int, default=20260809)
    ap.add_argument("--truth", default=None, help="真値（歪みなし）の書き出し先TIFF")
    args = ap.parse_args()

    size = args.size
    xs, ys, amp = build_scene(size, 40, args.seed)

    yy, xx = np.mgrid[0:size, 0:size].astype(np.float64)

    # 真値: 歪みなしで標本化したもの。
    truth = sample_scene(xs, ys, amp, args.sigma, xx, yy)
    lo, hi = truth.min(), truth.max()
    scale = 0.75 / max(hi - lo, 1e-9)
    offset = 0.12

    def normalize(v):
        return np.clip((v - lo) * scale + offset, 0.0, 1.0)

    if args.truth:
        write_tiff16(args.truth, normalize(truth))
        print(f"真値を書き出しました: {args.truth}")

    rng = np.random.RandomState(args.seed + 1)
    print(f"合成中: {size}x{size} x {args.frames} フレーム "
          f"(局所歪み ±{args.local}px / グローバル ±{args.global_shift}px)")

    with open(args.output, "wb") as f:
        write_header(f, size, size, args.frames, 16)
        for n in range(args.frames):
            gdx = rng.uniform(-args.global_shift, args.global_shift)
            gdy = rng.uniform(-args.global_shift, args.global_shift)
            if args.local > 0.0:
                ldx, ldy = warp_field(size, args.local, args.seed + 100 + n)
            else:
                ldx = ldy = 0.0
            # 歪んだ座標でシーンを直接評価する（画像のリサンプルではない）
            v = sample_scene(xs, ys, amp, args.sigma, xx + gdx + ldx, yy + gdy + ldy)
            v = normalize(v)
            v = v + args.noise * (rng.rand(size, size) - 0.5)
            v = np.clip(v, 0.0, 1.0)
            f.write((v * 65535.0 + 0.5).astype("<u2").tobytes())
            if (n + 1) % 50 == 0:
                print(f"  {n + 1}/{args.frames}", flush=True)

        # タイムスタンプトレーラ
        base = dotnet_ticks_now()
        for n in range(args.frames):
            f.write(struct.pack("<q", base + n * 100000))

    print(f"書き出しました: {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
