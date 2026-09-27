#!/bin/sh
# MOV・MP4 の読み込みの試験画像を作る（ffmpeg と macOS の AVFoundation を使う）。
#
#   sh tests/data/make_movie_fixtures.sh
#
# 64×48・30フレーム。左半分の明るさがフレーム番号 k を表す（20 + 7k）。右半分は固定の模様。
#   movie_h264_bframes.mp4 : H.264・Bフレーム2枚・キーフレーム間隔8（表示順とデコード順が違う）
#   movie_prores.mov       : ProRes 422
#   movie_h264_dated.mov   : H.264 に QuickTime 形式の作成日時 2024-01-19T02:24:06.250+0900
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="${TMPDIR:-/tmp}/lunastack_movie_fixtures_$$"
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT
python3 - "$WORK/raw.rgb" <<'PY'
import sys
w, h, n = 64, 48, 30
with open(sys.argv[1], "wb") as f:
    for k in range(n):
        v = 20 + 7 * k
        row = bytearray()
        for y in range(h):
            for x in range(w):
                if x < 32:
                    row += bytes([v, v, v])
                else:
                    t = (x * 5 + y * 3) % 200 + 30
                    row += bytes([t, t // 2, 255 - t])
        f.write(row)
PY
RAW="-f rawvideo -pix_fmt rgb24 -s 64x48 -r 30 -i $WORK/raw.rgb"
ffmpeg -loglevel error -y $RAW -c:v libx264 -bf 2 -g 8 -crf 1 -pix_fmt yuv420p "$HERE/movie_h264_bframes.mp4"
ffmpeg -loglevel error -y $RAW -c:v prores_ks -profile:v 2 "$HERE/movie_prores.mov"
ffmpeg -loglevel error -y $RAW -c:v libx264 -bf 2 -g 8 -crf 1 -pix_fmt yuv420p "$WORK/plain.mov"
clang++ -fobjc-arc "$HERE/set_quicktime_creationdate.mm" -framework AVFoundation -framework CoreMedia \
    -framework Foundation -o "$WORK/settime"
rm -f "$HERE/movie_h264_dated.mov"
"$WORK/settime" "$WORK/plain.mov" "$HERE/movie_h264_dated.mov" "2024-01-19T02:24:06.250+0900"
echo ok
