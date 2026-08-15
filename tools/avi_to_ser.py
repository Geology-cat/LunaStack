#!/usr/bin/env python3
"""AVI（またはffmpegが読める動画）をSER v3に変換する開発用ツール。

実キャプチャ由来の画素データでデコーダを検証するために使う。
SERを配布しているサイトはほとんどないが、AVIなら実データが手に入るため、
ffmpegで生画素に展開してSERコンテナに詰め直す。

重要な方針: **ffmpegに画素形式を変換させない**。
グレースケール素材をrgb24で受け取ると3倍に膨らむうえ補間が入り、
CFA配列があれば壊れる。元のpix_fmtをそのまま出させ、
対応するSERのColorIDを付けるだけにする。

使い方:
    python3 tools/avi_to_ser.py input.avi output.ser
    python3 tools/avi_to_ser.py input.avi output.ser --max-frames 500
    python3 tools/avi_to_ser.py input.avi output.ser --color-id 8   # CFAとして扱う
"""

import argparse
import json
import os
import struct
import subprocess
import sys
from datetime import datetime, timezone

HEADER_SIZE = 178
FILE_ID = b"LUCAM-RECORDER"

# SERのColorID
COLOR_MONO = 0
COLOR_BAYER_RGGB = 8
COLOR_BAYER_GRBG = 9
COLOR_BAYER_GBRG = 10
COLOR_BAYER_BGGR = 11
COLOR_RGB = 100
COLOR_BGR = 101

# ffmpegのpix_fmt -> (rawvideoで出力させるpix_fmt, ColorID, plane数, bit深度)
#
# ここに無い形式は「色か白黒か」だけ見て bgr24 / gray に寄せる。
# その場合は変換が入るので、その旨を必ず表示する。
PIX_FMT_MAP = {
    "gray": ("gray", COLOR_MONO, 1, 8),
    "gray8": ("gray", COLOR_MONO, 1, 8),
    "gray16le": ("gray16le", COLOR_MONO, 1, 16),
    "gray16be": ("gray16le", COLOR_MONO, 1, 16),  # SER側はリトルで持つ
    "bgr24": ("bgr24", COLOR_BGR, 3, 8),
    "rgb24": ("rgb24", COLOR_RGB, 3, 8),
    "bayer_rggb8": ("bayer_rggb8", COLOR_BAYER_RGGB, 1, 8),
    "bayer_grbg8": ("bayer_grbg8", COLOR_BAYER_GRBG, 1, 8),
    "bayer_gbrg8": ("bayer_gbrg8", COLOR_BAYER_GBRG, 1, 8),
    "bayer_bggr8": ("bayer_bggr8", COLOR_BAYER_BGGR, 1, 8),
}

GRAY_LIKE = {"gray", "gray8", "gray16le", "gray16be", "pal8", "monob", "monow"}


def probe(path):
    """入力の1本目の映像ストリームを調べる。"""
    out = subprocess.run(
        [
            "ffprobe", "-v", "error", "-select_streams", "v:0",
            "-show_entries",
            "stream=width,height,pix_fmt,codec_name,nb_frames,avg_frame_rate",
            "-of", "json", path,
        ],
        capture_output=True, text=True, check=True,
    ).stdout
    streams = json.loads(out).get("streams") or []
    if not streams:
        raise SystemExit(f"映像ストリームが見つかりません: {path}")
    return streams[0]


def parse_fps(value):
    """avg_frame_rate の "N/D" を float にする。壊れていれば None。"""
    try:
        num, _, den = value.partition("/")
        num, den = float(num), float(den or 1)
        return num / den if den and num else None
    except (TypeError, ValueError):
        return None


def dotnet_ticks(dt):
    """datetime を .NET ticks（0001-01-01からの100ns単位）に変換する。"""
    epoch_offset = 62135596800  # 0001-01-01 から 1970-01-01 までの秒数
    return int((dt.timestamp() + epoch_offset) * 10_000_000)


def build_header(color_id, width, height, depth, frame_count, ticks_utc,
                 observer, instrument, telescope):
    def fixed(text, size):
        return text.encode("ascii", "replace")[:size].ljust(size, b"\x00")

    return b"".join([
        FILE_ID,
        struct.pack("<i", 0),            # LuID
        struct.pack("<i", color_id),
        # LittleEndian: このフラグの意味はキャプチャソフト間で解釈が割れており
        # （0=ビッグと読む実装と、その逆がある）、受け手は盲信できない。
        # 8bitではそもそも無意味。16bitで書くデータは実際にリトルエンディアンなので
        # 「リトル」を主張する 1 を入れる。デコーダ側はこれを判定の材料にはするが
        # 最終的には画像の滑らかさから自動判定する（ser_decoder.hpp の ByteOrder 参照）。
        struct.pack("<i", 1),
        struct.pack("<i", width),
        struct.pack("<i", height),
        struct.pack("<i", depth),        # PixelDepthPerPlane
        struct.pack("<i", frame_count),
        fixed(observer, 40),
        fixed(instrument, 40),
        fixed(telescope, 40),
        struct.pack("<q", ticks_utc),    # DateTime（ローカル扱いだがUTCを入れる）
        struct.pack("<q", ticks_utc),    # DateTime_UTC
    ])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--max-frames", type=int, default=0,
                    help="先頭Nフレームだけ変換する（0で全部）")
    ap.add_argument("--color-id", type=int, default=None,
                    help="ColorIDを明示指定する（例: 8=RGGB）。"
                         "デバイヤー済みAVIを生CFAとして扱いたい場合など")
    ap.add_argument("--start-utc", default=None,
                    help="撮影開始時刻 ISO8601（既定はファイルの更新時刻）")
    ap.add_argument("--observer", default="")
    ap.add_argument("--instrument", default="")
    ap.add_argument("--telescope", default="")
    args = ap.parse_args()

    info = probe(args.input)
    width, height = int(info["width"]), int(info["height"])
    src_fmt = info["pix_fmt"]
    fps = parse_fps(info.get("avg_frame_rate", "")) or 30.0

    if src_fmt in PIX_FMT_MAP:
        raw_fmt, color_id, planes, depth = PIX_FMT_MAP[src_fmt]
        converted = raw_fmt != src_fmt
    elif src_fmt in GRAY_LIKE:
        raw_fmt, color_id, planes, depth = "gray", COLOR_MONO, 1, 8
        converted = True
    else:
        raw_fmt, color_id, planes, depth = "bgr24", COLOR_BGR, 3, 8
        converted = True

    if args.color_id is not None:
        color_id = args.color_id

    bytes_per_sample = 1 if depth <= 8 else 2
    frame_bytes = width * height * planes * bytes_per_sample

    print(f"入力      : {args.input}")
    print(f"コーデック: {info.get('codec_name')}  pix_fmt={src_fmt}  {width}x{height}  {fps:.2f}fps")
    print(f"出力形式  : rawvideo {raw_fmt} -> ColorID={color_id} "
          f"{planes}plane {depth}bit  1フレーム {frame_bytes:,} バイト")
    if converted:
        print(f"  ※ 注意: {src_fmt} -> {raw_fmt} の画素形式変換が入ります"
              f"（元データそのままではありません）")

    if args.start_utc:
        start = datetime.fromisoformat(args.start_utc).replace(tzinfo=timezone.utc)
    else:
        start = datetime.fromtimestamp(os.path.getmtime(args.input), timezone.utc)

    cmd = [
        "ffmpeg", "-v", "error", "-i", args.input,
        "-f", "rawvideo", "-pix_fmt", raw_fmt,
    ]
    if args.max_frames:
        cmd += ["-frames:v", str(args.max_frames)]
    cmd += ["-"]

    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, bufsize=1024 * 1024)
    count = 0
    try:
        with open(args.output, "wb") as out:
            # フレーム数は読み終わるまで確定しないので、後で書き戻す。
            out.write(build_header(color_id, width, height, depth, 0,
                                   dotnet_ticks(start), args.observer,
                                   args.instrument, args.telescope))
            while True:
                chunk = proc.stdout.read(frame_bytes)
                if len(chunk) < frame_bytes:
                    if chunk:
                        print(f"  末尾に半端な {len(chunk):,} バイト。切り捨てます。")
                    break
                out.write(chunk)
                count += 1
                if count % 200 == 0:
                    print(f"  {count} フレーム…", flush=True)

            # タイムスタンプトレーラ（フレーム数 × int64 .NET ticks）。
            # SERのオプション領域だが、これが無いと has_timestamps() が検証できない。
            for i in range(count):
                ts = dotnet_ticks(start) + int(i / fps * 10_000_000)
                out.write(struct.pack("<q", ts))

            out.seek(38)
            out.write(struct.pack("<i", count))
    finally:
        proc.stdout.close()
        proc.wait()

    if count == 0:
        raise SystemExit("フレームを1枚も読めませんでした。")

    size = os.path.getsize(args.output)
    expected = HEADER_SIZE + count * frame_bytes + count * 8
    print(f"完了      : {args.output}")
    print(f"  {count:,} フレーム / {size:,} バイト（期待値 {expected:,}）")
    if size != expected:
        print("  ※ サイズが期待値と一致しません", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
