[日本語](README.md) | **English**

# LunaStack

A native macOS stacking application for lunar and planetary imaging.
It combines local alignment with the MAP (Multiple Alignment Points) method used by AutoStakkert!
and wavelet sharpening in the style of RegiStax in a single app.

- **Download: [latest release](https://github.com/Geology-cat/LunaStack/releases/latest)** (the DMG contains the app, the user guide and an easy installer)
- Requirements: **macOS 10.12.6 (Sierra) or later / Intel and Apple Silicon** (HEVC video cannot be read on 10.12)
- Current version: **v1.1.1** (not yet tested on real 10.12 / 10.13 machines)
- How to use: [User guide (PDF, Japanese)](docs/manual/LunaStack_使い方ガイド.pdf)
- Design documents (Japanese): [Specification](docs/仕様書.md) / [Implementation plan](docs/実装計画書.md) / [UI design](docs/UI設計書.md) / [Development notes](docs/開発記録_2026-09-25.md) / [Development history](docs/開発履歴.md)

The app's interface is available in Japanese and English. It follows the macOS language at first;
choose **LunaStack → Preferences…** to switch. The user guide and design documents are in Japanese only.

## Features

- **Input**: SER / AVI (uncompressed, MJPEG) / MOV, MP4, M4V (H.264, HEVC, ProRes and others; read through AVFoundation, so pixel values can differ between OS versions; HEVC needs macOS 10.13 or later) /
  still-image sequences (TIFF, PNG, FITS, JPEG, camera RAW; a folder or a multiple selection)
- **Three stages**: [Quality Evaluation] → [Alignment] → [Stack]. Each stage stops when it finishes so you can check the result before moving on.
  [Run All with Current Settings] runs the three stages in a row with the settings as they are
  - Quality graph (timeline / quality order) and frame stepping in quality order
  - Local alignment per alignment point (AP) and per-AP frame selection (spatial lucky imaging)
  - Quality-weighted mean (default) / plain mean / sigma clipping, brightness normalization, drizzle upscaling (with a check of whether it is likely to help)
- **Mosaic** (lunar surface and similar): stitches already-stacked images (panels), or stacks the videos in the queue one panel at a time and
  then stitches them automatically. No panel order needs to be given. Small rotations (field rotation on an alt-az mount) and brightness differences are corrected, and seams are blended. The result is sent to Finish & Export.
  For the latter, choosing it in the menu changes the run-all button to [Run All + Mosaic with Current Settings], so you can check the settings before pressing it
- **Open an already-stacked image**: drop a single image file on the app (or use "Open With") to open it in Finish & Export
- **Input preprocessing**: frame range, processing area (part of a large sensor), Bayer pattern override, debayer method, dark and flat calibration
- **Adoption-rate guide**: splits the frames into two sets, stacks each separately, and compares at each adoption rate how much detail is reproduced in both,
  showing in a graph what percentage of this video is best to use ([Measure Adoption Rate] in the Stack tab)
- **Finishing** (non-destructive; the screen and the exported file go through the same pipeline): RGB channel alignment → 6-layer wavelets (with ring suppression) →
  black background (measures and subtracts the sky level, i.e. the camera offset) → white balance and saturation → levels (the same form as Photoshop) →
  rotate and flip. Cropping is drawn on the preview
- **Export**: 16-bit TIFF / 32-bit float TIFF / 32-bit float FITS / 16-bit PNG, recording of processing settings and capture time, WinJUPOS-style file names,
  export at several adoption rates at once
- **Saved analysis results**: quality evaluation and alignment results are saved in sidecar files (`.lstkq`, `.lstk`) and reloaded the next time the file is opened.
  Restacking with a different adoption rate takes seconds (Jupiter, 300 frames: 2.35 s versus 22.2 s including analysis)
- **Camera RAW**: read as linear data without color adjustment through the bundled LibRaw 0.22.2 (CDDL 1.0, `third_party/LibRaw`)
- Presets (`~/Library/Application Support/LunaStack/Presets/`), Japanese and English interface (follows the macOS language at first; switch in "LunaStack → Preferences…")

Every launch starts with an empty input queue and default settings (only the recent items, the export folder, which sections are expanded, and the interface language are kept).
For details of the interface see the [user guide](docs/manual/LunaStack_使い方ガイド.pdf); for the design see the [UI design document](docs/UI設計書.md) (both in Japanese).

## Build and test

Requires Xcode (command line tools) and CMake 3.20 or later.

```bash
cmake -S . -B .build/local && cmake --build .build/local -j8
ctest --test-dir .build/local --output-on-failure
```

`ctest` runs four tests.

1. `stackcore_tests` — unit and regression tests of the engine (245 cases)
2. `gui_selftest` — a self-check that runs the three GUI stages through the on-screen code paths with a synthetic SER.
   Skip it with `LUNASTACK_SKIP_GUI_TEST=1` where there is no GUI session
3. `localization_strings_valid` — syntax check of the Japanese and English resources
4. `availability_guard_fires` — **a test that expects the build to fail**. It confirms that code using an API newer than 10.12
   is a compile error (i.e. the 10.12 guard is working)

### Building the distribution

```bash
./scripts/build_app.sh   # dist/LunaStack.app (Universal; also runs the tests)
./scripts/make_dmg.sh    # dist/LunaStack-<version>.dmg
```

- Intermediate files go to `.build/universal/`; no other `LunaStack.app` is created at the repository root
- The DMG contains LunaStack.app, the user guide (PDF), the easy installer (`かんたんインストーラ.scpt`) and an alias to Applications. It is HFS+, so it opens on 10.12
- The easy installer (`scripts/dmg/かんたんインストーラ.applescript`) copies the app to the Applications folder,
  removes the quarantine attribute (com.apple.quarantine) from LunaStack only, and launches it for the first time (it does not change the system-wide Gatekeeper setting)
- To rebuild the user guide see [docs/manual/README.md](docs/manual/README.md) (Japanese)

Check the minimum OS of the shipped binaries with `vtool -show-build .build/universal/stackcli/stackcli`.
It is correct when x86_64 shows `LC_VERSION_MIN_MACOSX version 10.12` and arm64 shows `minos 11.0`.

## Using the CLI

A command line tool that uses the engine (`libstackcore`) directly. It does the same processing as the GUI from scripts.
Its messages are in Japanese.

```bash
# Show the header and measured values of a SER file (for diagnosing the decoder)
./.build/local/stackcli/stackcli info capture.ser

# Write a given frame as TIFF or 16-bit PNG
./.build/local/stackcli/stackcli extract capture.ser -f 100 -o frame100.png

# With the .fits extension, write 32-bit float FITS (normalized to 0.0–1.0)
./.build/local/stackcli/stackcli extract capture.ser -f 100 -o frame100.fits

# Stack with whole-image alignment only (fast)
./.build/local/stackcli/stackcli stack capture.ser -o stacked.tif --top 25

# Stack with local alignment per alignment point (add --stack-mode weighted to match the GUI default)
./.build/local/stackcli/stackcli mapstack capture.ser -o stacked.tif --top 25 --ap-top 25 --stack-mode weighted

# Stitch already-stacked images (panels) into a mosaic (in any order)
./.build/local/stackcli/stackcli mosaic panel1.tif panel2.tif panel3.tif -o mosaic.tif
```

Main options:

| Option | Description |
|---|---|
| `--endian auto\|little\|big` | Byte order of 16-bit samples (default auto) |
| `--bit-depth <N>` | Override the bit depth used for normalization |
| `--float` | Write 32-bit float TIFF |
| Output name `.png` | Write a deterministic 16-bit PNG |
| Output name `.fits` / `.fit` | Write 32-bit float FITS for PixInsight and similar |
| `--raw-cfa` | Output the raw CFA without debayering |
| `--frames <start:end>` | Range of frames to use (1-based, end inclusive) |
| `--bayer mono\|rggb\|grbg\|gbrg\|bggr` | Specify the color format manually |
| `--debayer bilinear\|mhc` | Debayer method (mhc = Malvar-He-Cutler) |
| `--dark <source>` / `--flat <source>` | Dark and flat calibration (video, still image or folder) |
| `--metadata` / `--object <name>` | Record processing settings and capture time in the file |

Besides videos, the input can be a folder of still images (TIFF, PNG, FITS, JPEG, camera RAW).
Numbers in file names are sorted in natural order (`img2` < `img10`).

Options of `mapstack` (those of `stack` can be used too):

| Option | Description |
|---|---|
| `--ap-size <px>` | AP size 32/48/64/96/128/200 (default: suggested automatically; 64 px for large targets such as the lunar surface) |
| `--ap-top <percent\|auto>` | Percentage of frames adopted per AP (default 25). `auto` measures the guide and uses it |
| `--suggest-top` | Measure and print the adoption-rate guide |
| `--ap-count <count>` | Number of frames adopted per AP, given directly |
| `--quality gradient\|frequency` | Gradient energy / FFT band power ratio |
| `--stack-mode mean\|weighted\|sigma` | Plain mean (CLI default) / quality-weighted mean (GUI default) / sigma clipping |
| `--no-normalize` | Disable brightness normalization |
| `--search-radius <px>` | Radius of the local search (default 16) |
| `--min-score <value>` | Lower limit of ZNCC. APs below it are interpolated from neighbors (default 0.5) |
| `--ap-gradient <ratio>` / `--ap-level <ratio>` | Gradient and brightness thresholds for placing APs (defaults 0.6 and 0.05) |
| `--ref-passes <N>` | Number of iterative reference refinements (default 2; 1 means no refinement) |
| `--sidecar <path.lstk>` | Save the analysis results to a sidecar |
| `--reuse-sidecar` | Read the analysis results from the sidecar and redo only the stacking |
| `--low-memory` | Reduce peak RSS (10,000 frames: 6105 MB → 358 MB, about 18% slower) |

Options of `stack`:

| Option | Description |
|---|---|
| `--top <percent>` | Percentage of best frames to stack (default 25) |
| `--mode auto\|planet\|lunar` | Kind of target (default auto; decided from the fraction of dark area in the reference frame) |
| `--min-similarity <value>` | Absolute lower limit of similarity (ZNCC) to the reference (default 0.5) |
| `--outlier-k <value>` | Strictness of the similarity outlier test (default 6.0) |
| `--max-shift <pixels>` | Upper limit of the allowed shift (default 1/4 of the short side) |
| `--limit <N>` | Process only the first N frames (for quick checks) |

### How `stack` works

1. **Quality evaluation of all frames** — gradient energy after a Gaussian blur of σ≈1 (specification §4.3)
2. **Global alignment** — planet mode finds a coarse position by thresholding and brightness centroid, then phase correlation;
   wide-field mode uses phase correlation over the whole frame. Tracking failures are excluded by similarity (ZNCC) to the reference and by the size of the shift
3. **Stacking** — the best N% of the accepted frames are averaged at integer shifts with brightness normalization

The same input and options reproduce the output byte for byte (the stacking order is fixed to ascending frame number).

### When the image looks broken

1. Look at **[バイトオーダー]** (byte order) in `info`. A message that it disagrees with the header
   is not an error (capture programs interpret this flag differently, so it is detected automatically).
   If the image looks like pure noise, compare `--endian little` and `--endian big` explicitly.
2. Look at **実効ビット幅** (effective bit width) in `info`. If the header says 16 bits but the maximum is 4095 or less
   (right-aligned 12-bit data), pass `--bit-depth 12` (otherwise the image is extremely dark).
   Left-aligned data whose lower 4 bits are zero is already on the 16-bit scale, so leave it as 16 bits.

## Directory layout

```text
libstackcore/     Engine (C++17 static library, UI independent)
  include/stackcore/
  src/image/        frame_buffer, debayer (bilinear / MHC), calibration, quality, resample
  src/registration/ phase_correlate, global_aligner, zncc_matcher
  src/map/          ap_placer, local_aligner
  src/stack/        simple_stacker, frame_selector, windowed_stacker, drizzle
  src/pipeline/     global_stage, map_pipeline
  src/post/         wavelet, finishing (channel alignment, color, geometry, deringing, finishing pipeline)
  src/io/           ser_decoder, avi_decoder, jpeg_decoder, image_reader, inflate,
                    video_source (preprocessing wrapper, still sequences), raw_reader (CR2, DNG),
                    libraw_reader (camera RAW through LibRaw), tiff/png/fits_writer,
                    metadata, sidecar, mapped_file
LunaStackApp/     GUI (AppKit, MRC, no xib)
  src/MainWindowController.mm and +Layout / +Queue / +Settings / +Jobs / +Preview /
      +Finishing / +SelfCheck (instance variables are in MainWindowController_Private.h)
stackcli/         CLI
third_party/      Bundled libraries (LibRaw 0.22.2, unmodified, CDDL 1.0; source and SHA-256 in README.LunaStack.md)
scripts/          Scripts that build the app and the DMG (the easy installer is in dmg/)
.build/           CMake intermediate files (not in the repository)
dist/             Latest verified Universal app and DMG (not in the repository)
tests/            Tests (own minimal harness, no external dependencies)
  data/             Still test images written by other software (generated with PIL, tifffile, tiffcp)
  tools/            Synthetic SER generator for the GUI self-check
tools/            Development scripts (not part of the build)
docs/             Specification, implementation plan, UI design, development notes and history (Japanese)
  manual/           User guide (LaTeX; the PDF and the scripts that take screenshots and generate figures)
sample-data/      Real data for verification (huge; not in the repository)
```

## Preparing sample data for verification

For checking the decoders with real captures. Almost no site distributes `.SER` files,
so download an AVI and convert it to SER. ffmpeg is required.

```bash
mkdir -p sample-data && cd sample-data
curl -L -C - -o jupiter_pipp.avi https://jupiter-imagery.s3.amazonaws.com/2021-07-08-1050_9-Jupiter_pipp.avi
python3 ../tools/avi_to_ser.py jupiter_pipp.avi jupiter.ser --start-utc 2021-07-08T10:50:00
```

2.78 GB (rawvideo bgr24 / 448x448 / 4617 frames / 92 fps).
The converted SER is about the same size, so about 6 GB of free space is needed in total.
The full-length version (10.1 GB) is at the same location.

`avi_to_ser.py` **does not let ffmpeg convert the pixel format**.
Receiving grayscale material as rgb24 triples its size and adds interpolation,
and a CFA pattern would be destroyed, making the Bayer path impossible to verify.
It has ffmpeg output the raw pixels in the original `pix_fmt` and only attaches the matching ColorID
(`gray` → MONO, `bgr24` → BGR, `bayer_*` → the matching CFA ID).
It also writes the timestamp trailer so that `has_timestamps()` can be verified.

To make a sample for the 16-bit path (the source is 8-bit, so the depth is not real,
but it works as real data for the byte-order detection):

```bash
# The AVI container cannot hold gray16le. ffmpeg only warns and
# writes a file garbled into rgb555le, so use NUT
ffmpeg -i jupiter_pipp.avi -frames:v 200 -pix_fmt gray16le -c:v rawvideo jup16.nut
python3 ../tools/avi_to_ser.py jup16.nut jup16.ser
```

To scan all frames and measure mmap behavior and throughput:

```bash
c++ -std=c++17 -O2 -Ilibstackcore/include tools/ser_sweep.cpp \
    .build/local/libstackcore/libstackcore.a -framework Accelerate -o /tmp/ser_sweep
/tmp/ser_sweep sample-data/jupiter.ser
```
