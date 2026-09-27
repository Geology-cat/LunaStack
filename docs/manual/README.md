# LunaStack 使い方ガイド（初心者向け）

`LunaStack_使い方ガイド.pdf` が完成版。LaTeX（LuaLaTeX・jlreq・原ノ味フォント）で組む。

## 構成

| パス | 内容 |
|---|---|
| `guide.tex` | 本体（表紙・目次・各章の読み込み） |
| `chapters/*.tex` | 各章 |
| `lsmanual.sty` | スクリーンショットに番号・枠・矢印を重ねるマクロ、囲み記事・設定項目の体裁 |
| `figures/` | 図（`tools/figures.py` が作る。画像と `*.coords.tex`＝部品の位置） |
| `tools/make_shots.sh` | アプリを場面ごとに起動して画面を撮る（`build/raw/` へ） |
| `tools/figures.py` | 撮った画面を切り出し、部品の位置を図の座標に直す |

## 作り直す手順

```bash
./scripts/build_app.sh                 # または cmake --build .build/local
docs/manual/tools/make_shots.sh        # 画面を撮る（5分ほど。場面名を付けるとその場面だけ）
python3 docs/manual/tools/figures.py   # 図を作る（図名を付けるとその図だけ）
cd docs/manual && latexmk               # build/guide.pdf ができる
cp build/guide.pdf LunaStack_使い方ガイド.pdf
```

- 撮影には `sample-data/` の素材を使う（リポジトリには入っていない。`make_shots.sh` の先頭に一覧）。
  撮影のたびに、その素材の解析キャッシュ（`.lstk` など）を消して同じ状態から撮る。
- 画面の撮影はアプリの自己検証用の環境変数（`LUNASTACK_SNAPSHOT` など）を使う。説明書のために次を足した:
  `LUNASTACK_LAYOUT`（部品の種類・文言・設定欄・位置を JSON に書く）、`LUNASTACK_INSPECTOR_SHOT`（設定パネルを縦に全部つないだ画像）、
  `LUNASTACK_TAB_NORMAL`（タブを選ぶとき見出しの開閉どおりに見せる）、`LUNASTACK_GRAPH`（品質グラフの時系列／品質順）、
  `LUNASTACK_FINISH` の `wavelet=`・`denoise=`（レイヤーごとの値）。
- 見出しの開閉は利用者の設定を書き換えず、起動引数 `-section.<key> YES` で上書きする。
- 番号や枠の位置は `figures.py` が部品の位置から求めるので、画面が変わっても撮り直せば追従する。
  番号の置き場所（部品の左上・右など）は各章の `\mk[位置]{部品}{番号}` で決める。
- 組み上がったら `pdftoppm -r 60 -png build/guide.pdf build/pages/p` で全ページを画像にして、番号の重なりなどを目で確かめる。
