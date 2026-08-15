#!/usr/bin/env python3
"""既存のSERのフレームを繰り返して、指定フレーム数のSERを作る。

用途は**規模の検証**であって画質の検証ではない。
「1万フレーム入力でメモリ上限を維持する」（実装計画書 M3のAC）を確かめるには、
1万フレームぶんの実データが要るが、そのために合成データを作ると
生成だけで1時間半かかる（斑点の重ね合わせを全画素で評価するため）。
既存の実データを繰り返せば数十秒で用意でき、
**メモリとスループットの検証には十分**である。

同じフレームが何度も現れるので、スタック結果の画質やS/Nには意味がない。
そちらを測るときはこのファイルを使わないこと。

使い方:
    python3 tools/ser_repeat.py in.ser out.ser --frames 10000
"""

import argparse
import os
import struct
import sys

HEADER_SIZE = 178
CHUNK = 8 * 1024 * 1024


def read_header(f):
    f.seek(0)
    h = f.read(HEADER_SIZE)
    if len(h) < HEADER_SIZE or not h.startswith(b"LUCAM-RECORDER"):
        raise SystemExit("SERファイルではないようです（識別子が違います）")
    color_id = struct.unpack("<i", h[18:22])[0]
    width = struct.unpack("<i", h[26:30])[0]
    height = struct.unpack("<i", h[30:34])[0]
    depth = struct.unpack("<i", h[34:38])[0]
    frames = struct.unpack("<i", h[38:42])[0]
    planes = 3 if color_id in (100, 101) else 1
    bps = 1 if depth <= 8 else 2
    frame_bytes = width * height * planes * bps
    return h, width, height, depth, frames, frame_bytes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--frames", type=int, required=True)
    args = ap.parse_args()

    with open(args.input, "rb") as src:
        header, w, h, depth, src_frames, frame_bytes = read_header(src)
        total_out = HEADER_SIZE + frame_bytes * args.frames + 8 * args.frames
        print(f"入力: {w}x{h} {depth}bit {src_frames} フレーム "
              f"(1フレーム {frame_bytes:,} バイト)")
        print(f"出力: {args.frames} フレーム / {total_out / 1e9:.2f} GB")

        free = os.statvfs(os.path.dirname(os.path.abspath(args.output)))
        free_bytes = free.f_bavail * free.f_frsize
        if free_bytes < total_out * 1.05:
            raise SystemExit(f"空き容量が足りません（必要 {total_out/1e9:.1f} GB / "
                             f"空き {free_bytes/1e9:.1f} GB）")

        # フレーム数だけ書き換えたヘッダ。
        new_header = bytearray(header)
        new_header[38:42] = struct.pack("<i", args.frames)

        with open(args.output, "wb") as dst:
            dst.write(new_header)

            written = 0
            while written < args.frames:
                take = min(src_frames, args.frames - written)
                src.seek(HEADER_SIZE)
                remaining = frame_bytes * take
                while remaining > 0:
                    buf = src.read(min(CHUNK, remaining))
                    if not buf:
                        raise SystemExit("入力が途中で終わりました")
                    dst.write(buf)
                    remaining -= len(buf)
                written += take
                print(f"  {written}/{args.frames} フレーム", flush=True)

            # タイムスタンプトレーラ。等間隔で埋める。
            base = 638000000000000000
            step = 100000
            block = bytearray()
            for i in range(args.frames):
                block += struct.pack("<q", base + i * step)
                if len(block) >= CHUNK:
                    dst.write(block)
                    block = bytearray()
            if block:
                dst.write(block)

    print(f"書き出しました: {args.output} "
          f"({os.path.getsize(args.output) / 1e9:.2f} GB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
