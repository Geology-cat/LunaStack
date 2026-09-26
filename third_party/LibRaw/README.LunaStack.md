# LibRaw（同梱）

- 版: 0.22.2（無改変）
- 入手元: https://www.libraw.org/data/LibRaw-0.22.2.tar.gz
- SHA-256: de86b035655accff8d4010f1a221fdf50d353cb7b1422ba26f14a0db92612cfa
- 同梱したもの: `libraw/` `internal/` `src/`（`src/Makefile` を除く）と、COPYRIGHT・LICENSE.CDDL・LICENSE.LGPL・README.md・Changelog.txt
- ライセンス: LibRaw は LGPL 2.1 と CDDL 1.0 の選択制。LunaStack は **CDDL 1.0** の条件で使う
  （静的リンクしてもアプリ本体のライセンスには及ばない。LibRaw のソースと著作権表示を同梱する）。
  アプリには LICENSE.CDDL と COPYRIGHT を Resources/ThirdParty/LibRaw に入れる
- 使い方: `libstackcore/src/io/libraw_reader.cpp` から、カメラRAW（CR2・DNG を含むすべて）の展開（`unpack`）と
  機種ごとの切り抜き・黒・白の情報だけを使う。Deflate の DNG のため `USE_ZLIB` を付け、macOS 標準の libz をリンクする。色補間・色変換・ガンマ（`dcraw_process`）は使わない
- 更新するとき: 版を上げると黒・白・切り抜きの表が変わり、同じRAWでも出力が変わることがある。
  上げたら開発記録に書き、サンプルで値を比べること

## 配布物に付ける表示（CDDL 1.0 §3.1）

LunaStack は LibRaw 0.22.2（Copyright (C) 2008-2025 LibRaw LLC）を無改変で含みます。
LibRaw は CDDL 1.0 の条件で使用しています（LICENSE.CDDL）。LibRaw のソースコードは
https://www.libraw.org/ と、LunaStack のソース一式の third_party/LibRaw から入手できます。
