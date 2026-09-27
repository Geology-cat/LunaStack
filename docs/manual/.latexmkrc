# LunaStack 使い方ガイド: docs/manual で latexmk を実行する
$pdf_mode = 4;          # LuaLaTeX
$lualatex = 'lualatex -interaction=nonstopmode -halt-on-error -file-line-error %O %S';
$out_dir = 'build';
@default_files = ('guide.tex');
