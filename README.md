# LunaStack

macOS ネイティブの月・惑星スタッキングソフトウェア。
AutoStakkert! の MAP（Multiple Alignment Points）方式による局所アライメントと、
RegiStax のウェーブレットシャープニングを、1つのアプリにまとめた。

- **ダウンロード: [最新版のリリースページ](https://github.com/Geology-cat/LunaStack/releases/latest)**（DMG に、アプリ・使い方ガイド・かんたんインストーラが入っています）
- 対応環境: **macOS 10.13 (High Sierra) 以降 / Intel・Apple Silicon 両対応**
- 現在の版: **v1.0.0**（初期リリース。Apple シリコン実機での精度の検証と、10.13 実機での動作確認は未実施）
- 使い方: [使い方ガイド（PDF）](docs/manual/LunaStack_使い方ガイド.pdf)
- 設計文書: [仕様書](docs/仕様書.md) / [実装計画書](docs/実装計画書.md) / [UI設計書](docs/UI設計書.md) / [開発記録](docs/開発記録_2026-09-25.md) / [開発履歴](docs/開発履歴.md)

## 主な機能

- **入力**: SER / AVI（非圧縮・MJPEG）/ MOV・MP4・M4V（H.264・HEVC・ProRes など。AVFoundation で読むので、OSの版で画素が変わることがある）/
  静止画連番（TIFF・PNG・FITS・JPEG・カメラのRAW。フォルダまたは複数選択）
- **3つの工程**: ［品質評価］→［アライメント］→［スタック］。各工程は終わると止まり、結果を確かめてから次へ進む
  - 品質グラフ（時系列／品質順）と、品質順のコマ送り
  - 位置合わせ領域（AP）ごとの局所アライメントと、APごとのフレーム選別（spatial lucky imaging）
  - 単純平均／品質重み付き平均／σクリップ、輝度正規化、ドリズル拡大（効きそうかの診断つき）
- **入力の前処理**: フレーム範囲、処理範囲（大きなセンサーの一部だけ）、Bayer 配列の指定、デバイヤー方式、ダーク・フラット補正
- **仕上げ**（非破壊。画面と書き出しは同じ処理系）: RGBチャンネル合わせ → 6レイヤーのウェーブレット（輪抑制つき）→
  ホワイトバランス・彩度 → レベル補正（Photoshop と同じ形）→ 回転・反転。プレビューの上で描く切り抜き
- **書き出し**: 16bit TIFF / 32bit float TIFF / 32bit float FITS / 16bit PNG、処理条件と撮影時刻の記録、WinJUPOS 形式の名前、
  複数の採用率のまとめ書き出し
- **解析結果の保存**: 品質評価とアライメントの結果をサイドカー（`.lstkq`・`.lstk`）に保存し、次に開いたときに読み直す。
  採用率を変えた再スタックは数秒（木星300フレームで、解析込み 22.2 秒に対し 2.35 秒）
- **カメラのRAW**: 同梱の LibRaw 0.22.2（CDDL 1.0、`third_party/LibRaw`）で、色の調整をしないリニアのまま読む
- プリセット（`~/Library/Application Support/LunaStack/Presets/`）、日本語・英語の画面

起動するたびに、入力キューと設定は初期状態から始まる（残すのは最近使った項目、書き出し先のフォルダ、見出しの開閉だけ）。
画面の詳しい使い方は[使い方ガイド](docs/manual/LunaStack_使い方ガイド.pdf)、設計は [UI設計書](docs/UI設計書.md) にある。

## ビルドとテスト

Xcode（コマンドラインツール）と CMake 3.20 以降が要る。

```bash
cmake -S . -B .build/local && cmake --build .build/local -j8
ctest --test-dir .build/local --output-on-failure
```

`ctest` は4つを実行する。

1. `stackcore_tests` — エンジンのユニット・リグレッションテスト（237件）
2. `gui_selftest` — 合成SERでGUIの3工程を画面の経路で通す自己検証。
   GUIセッションの無い環境では `LUNASTACK_SKIP_GUI_TEST=1` で飛ばせる
3. `localization_strings_valid` — 日本語・英語リソースの構文検証
4. `availability_guard_fires` — **ビルドが失敗することを期待するテスト**。10.13 より新しいAPIを使ったコードが
   コンパイルエラーになる（＝10.13対応のガードが効いている）ことを確かめる

### 配布物を作る

```bash
./scripts/build_app.sh   # dist/LunaStack.app（Universal。テストも実行）
./scripts/make_dmg.sh    # dist/LunaStack-<版>.dmg
```

- 中間生成物は `.build/universal/` に集め、リポジトリ直下に別の `LunaStack.app` を作らない
- DMG の中身は LunaStack.app・使い方ガイド（PDF）・かんたんインストーラ.scpt・Applications への別名。HFS+ なので 10.13 でも開ける
- かんたんインストーラ（`scripts/dmg/かんたんインストーラ.applescript`）は、アプリケーションフォルダへのコピー、
  LunaStack だけの隔離属性（com.apple.quarantine）の解除、最初の起動までを行う（macOS 全体の Gatekeeper の設定は変えない）
- 使い方ガイドの作り直しは [docs/manual/README.md](docs/manual/README.md)

出荷バイナリの最低OSは `vtool -show-build .build/universal/stackcli/stackcli` で確かめる。
x86_64 は `LC_VERSION_MIN_MACOSX version 10.13`、arm64 は `minos 11.0` になっていれば正しい。

## CLIの使い方

エンジン（`libstackcore`）をそのまま使うコマンドラインツール。GUI と同じ処理を、スクリプトから行える。

```bash
# SERファイルのヘッダと実測値を表示（デコーダの診断用）
./.build/local/stackcli/stackcli info capture.ser

# 指定フレームをTIFFまたは16bit PNGに書き出す
./.build/local/stackcli/stackcli extract capture.ser -f 100 -o frame100.png

# 拡張子 .fits で32bit float FITSに書き出す（0.0〜1.0に正規化）
./.build/local/stackcli/stackcli extract capture.ser -f 100 -o frame100.fits

# 画像全体の位置合わせだけでスタックする（速い）
./.build/local/stackcli/stackcli stack capture.ser -o stacked.tif --top 25

# 位置合わせ領域ごとの局所アライメントでスタックする（GUI の既定と同じ）
./.build/local/stackcli/stackcli mapstack capture.ser -o stacked.tif --top 25 --ap-top 10
```

主なオプション:

| オプション | 説明 |
|---|---|
| `--endian auto\|little\|big` | 16bitサンプルのバイトオーダー（既定 auto） |
| `--bit-depth <N>` | 正規化に使うビット深度を上書き |
| `--float` | 32bit float TIFFで書き出す |
| 出力名 `.png` | 決定論的な16bit PNGで書き出す |
| 出力名 `.fits` / `.fit` | PixInsight等向けの32bit float FITSで書き出す |
| `--raw-cfa` | Bayerをデバイヤーせず生のCFAのまま出力 |
| `--frames <開始:終了>` | 使うフレームの範囲（1始まり、終了を含む） |
| `--bayer mono\|rggb\|grbg\|gbrg\|bggr` | 色形式を手動で指定する |
| `--debayer bilinear\|mhc` | デバイヤー方式（mhc = Malvar-He-Cutler） |
| `--dark <素材>` / `--flat <素材>` | ダーク・フラット補正（動画・静止画・フォルダ） |
| `--metadata` / `--object <名前>` | 処理条件と撮影時刻をファイルに記録する |

入力には動画のほか、静止画（TIFF・PNG・FITS・JPEG・カメラのRAW）の入ったフォルダを指定できる。
ファイル名の数字は自然順（`img2` < `img10`）に並べる。

`mapstack` のオプション（`stack` のものも使える）:

| オプション | 説明 |
|---|---|
| `--ap-size <px>` | APサイズ 32/48/64/96/128/200（既定 自動提案） |
| `--ap-top <割合>` | AP別に採用するフレームの割合（既定 10） |
| `--ap-count <枚数>` | AP別の採用枚数を直接指定 |
| `--quality gradient\|frequency` | 勾配エネルギー／FFT周波数帯パワー比 |
| `--stack-mode mean\|weighted\|sigma` | 単純平均／品質重み付き平均／σクリップ |
| `--no-normalize` | 輝度正規化を無効にする |
| `--search-radius <px>` | 局所探索の半径（既定 16） |
| `--min-score <値>` | ZNCCの下限。下回るAPは近傍から補間（既定 0.5） |
| `--ap-gradient <比>` / `--ap-level <比>` | AP採用の勾配・輝度しきい値 |
| `--ref-passes <N>` | 参照の反復精密化の回数（既定 2。1で精密化なし） |
| `--sidecar <path.lstk>` | 解析結果をサイドカーに保存する |
| `--reuse-sidecar` | サイドカーから解析結果を読み、加算だけやり直す |
| `--low-memory` | 最大RSSを抑える（1万フレームで 6105MB → 358MB、時間は約18%増） |

`stack` のオプション:

| オプション | 説明 |
|---|---|
| `--top <割合>` | 品質上位何%を加算するか（既定 25） |
| `--mode auto\|planet\|lunar` | 対象の種類（既定 auto。参照フレームの暗部の割合で判定） |
| `--min-similarity <値>` | 参照との類似度(ZNCC)の絶対下限（既定 0.5） |
| `--outlier-k <値>` | 類似度の外れ値判定の厳しさ（既定 6.0） |
| `--max-shift <画素>` | 許容する変位の上限（既定 短辺の1/4） |
| `--limit <N>` | 先頭Nフレームだけ処理する（動作確認用） |

### stack の処理の流れ

1. **全フレームの品質評価** — σ≈1のガウスぼかし後の勾配エネルギー（仕様書 §4.3）
2. **グローバルアライメント** — 惑星モードは閾値二値化＋輝度重心で粗位置を出してから位相相関、
   広視野モードは全面の位相相関。参照との類似度(ZNCC)と変位の大きさで追跡失敗を除外する
3. **加算** — 採用フレームの品質上位N%を、輝度正規化しつつ整数変位で平均する

同じ入力・同じオプションなら出力はバイト単位で再現する（加算順をフレーム番号昇順に固定）。

### 画像が破綻して見えるときの診断手順

1. `info` の **[バイトオーダー]** を見る。「ヘッダの主張と食い違っています」と
   出ていても異常ではない（キャプチャソフト間でこのフラグの解釈が割れているため
   自動判定している）。画像がノイズにしか見えない場合は
   `--endian little` / `--endian big` を明示指定して比べる。
2. `info` の **実効ビット幅** を見る。ヘッダが16bitなのに最大値が4095以下の
   右詰め12bitなら、`--bit-depth 12` を指定する（指定しないと画像が極端に暗くなる）。
   下位4bitが0の左詰めデータは既に16bitスケールなので、上書きせず16bitとして扱う。

## ディレクトリ構成

```text
libstackcore/     エンジン（C++17静的ライブラリ、UI非依存）
  include/stackcore/
  src/image/        frame_buffer, debayer（bilinear / MHC）, calibration, quality, resample
  src/registration/ phase_correlate, global_aligner, zncc_matcher
  src/map/          ap_placer, local_aligner
  src/stack/        simple_stacker, frame_selector, windowed_stacker, drizzle
  src/pipeline/     global_stage, map_pipeline
  src/post/         wavelet, finishing（チャンネル合わせ・色・形・デリンギング・仕上げの処理系）
  src/io/           ser_decoder, avi_decoder, jpeg_decoder, image_reader, inflate,
                    video_source（前処理ラッパー・静止画連番）, image_reader, raw_reader（CR2・DNG）,
                    libraw_reader（LibRaw経由のカメラRAW）, tiff/png/fits_writer,
                    metadata, sidecar, mapped_file
LunaStackApp/     GUI（AppKit・MRC・xibなし）
  src/MainWindowController.mm と +Layout / +Queue / +Settings / +Jobs / +Preview /
      +Finishing / +SelfCheck（インスタンス変数は MainWindowController_Private.h）
stackcli/         CLI
third_party/      同梱ライブラリ（LibRaw 0.22.2・無改変・CDDL 1.0。README.LunaStack.md に入手元とSHA-256）
scripts/          アプリ・DMG の作成スクリプト（dmg/ にかんたんインストーラ）
.build/           CMake中間生成物（リポジトリには含めない）
dist/             最新の検証済みUniversalアプリと DMG（リポジトリには含めない）
tests/            テスト（自前の最小ハーネス、外部依存なし）
  data/             他ソフトが書いた静止画の試験画像（PIL・tifffile・tiffcpで生成）
  tools/            GUI自己検証用の合成SER生成
tools/            開発用スクリプト（ビルドには含まれない）
docs/             仕様書・実装計画書・UI設計書・開発記録・開発履歴
  manual/           使い方ガイド（LaTeX。PDF・画面の撮影と図の生成スクリプト）
sample-data/      検証用の実データ（巨大。リポジトリには含めない）
```

## 検証用サンプルデータの用意

実撮影データでデコーダを確かめたいとき。`.SER` を配布しているサイトは
ほぼ無いので、AVIを落としてSERに変換する。ffmpegが必要。

```bash
mkdir -p sample-data && cd sample-data
curl -L -C - -o jupiter_pipp.avi https://jupiter-imagery.s3.amazonaws.com/2021-07-08-1050_9-Jupiter_pipp.avi
python3 ../tools/avi_to_ser.py jupiter_pipp.avi jupiter.ser --start-utc 2021-07-08T10:50:00
```

2.78GB（rawvideo bgr24 / 448x448 / 4617フレーム / 92fps）。
変換後のSERはほぼ同サイズになるので、合計6GB程度の空きが要る。
全長版（10.1GB）も同じ場所にある。

`avi_to_ser.py` は **ffmpegに画素形式を変換させない**。
グレースケール素材をrgb24で受け取ると3倍に膨らむうえ補間が入り、
CFA配列があれば壊れて、Bayer経路が検証不能になるため。
元の `pix_fmt` のまま生画素を出させ、対応するColorIDを付けるだけにしている
（`gray`→MONO、`bgr24`→BGR、`bayer_*`→対応するCFA ID）。
`has_timestamps()` を検証できるようタイムスタンプトレーラも書く。

16bit経路を試すサンプルを作る（元が8bitなので深度の中身は本物ではないが、
バイトオーダー判定の入力としては実データとして機能する）:

```bash
# AVIコンテナはgray16leを格納できない。ffmpegは警告するだけで
# rgb555leに化けたファイルを書くので、NUTを使うこと
ffmpeg -i jupiter_pipp.avi -frames:v 200 -pix_fmt gray16le -c:v rawvideo jup16.nut
python3 ../tools/avi_to_ser.py jup16.nut jup16.ser
```

全フレームを走査してmmapの挙動とスループットを測る:

```bash
c++ -std=c++17 -O2 -Ilibstackcore/include tools/ser_sweep.cpp \
    .build/local/libstackcore/libstackcore.a -framework Accelerate -o /tmp/ser_sweep
/tmp/ser_sweep sample-data/jupiter.ser
```
