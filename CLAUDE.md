# Hikariuni

Affinity 向けの Photoshop 互換フィルタープラグイン（`.8bf`）。光源を見つけて光芒を描く。

## 仕様ドキュメント

- 決まった仕様は `docs/spec.md` に書く。
- 仕様が変わる変更（挙動、パラメーター、アルゴリズム、ビルド方法、フォルダ構成、対応環境、命名など）をしたら、**同じ変更の中で `docs/spec.md` も更新する**。コードと spec が食い違ったまま作業を終えない。
- まだ決まっていないことは spec に書かない。決まった時点で追記する。

## 構成とビルド

- ソリューションは `src/hikariuni.slnx`、プロジェクトは `src/<プロジェクト名>/<プロジェクト名>.vcxproj`。新しいプロジェクト（テストなど）も同じ形で追加する。
- `PHOTOSHOP_SDK_PATH` に Photoshop Plug-in SDK の `pluginsdk` フォルダが必要。SDK はリポジトリに入れない。
- コマンドラインでのビルド（VS 2026 の MSBuild）:
  ```
  "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" src\hikariuni.slnx -restore -p:RestorePackagesConfig=true -p:Configuration=Release -p:Platform=x64
  ```
  出力は `src\x64\Release\hikariuni.8bf`。
- テストは GoogleTest（VS の Google Test テンプレートと同じ NuGet パッケージ、`packages.config`）。vcpkg は使わない。`hikariuni.test` はビルド後に自動実行され、失敗するとビルドが失敗する。
- 光源の検出・描画（`effect.*`）は Windows と SDK に依存させない。変更したら `hikariuni.test/effect_test.cpp` にテストを足す。

## コードスタイル

- C++ は `.clang-format` に従い、編集したら整形する。
  - clang-format は VS 同梱: `C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\Llvm\x64\bin\clang-format.exe`
  - `*.r` は Rez 構文なので clang-format をかけない。
- SDK のヘッダーは `#include <PIFilter.h>` のように `<>` で読み込む（`.r` でも同じ）。
- C++23 で書く。画素の添字は `std::mdspan`（`RgbImage::view()` / `LightBuffer::view()` の `[y, x, c]`）、アルゴリズムは `std::ranges`、文字列の整形は `std::format`、構造体の初期化は designated initializers を使う。
  - GoogleTest のマクロの引数に `v[y, x, c]` を書くときは `(v[y, x, c])` と括弧で囲む（マクロの引数がカンマで分かれてしまうため）。
  - `view()` は元のデータを指すので、一時オブジェクトから取らない（`Resample(...).view()` はぶら下がる）。
- ソースは UTF-8。コメントは日本語。

## 気を付けること

- Affinity でプラグインを読み込んでいる間は `src\x64\<構成>\hikariuni.8bf` を上書きできない（リンクが LNK1104 で失敗する）。確かめるだけなら `-p:OutDir=...` で別のフォルダに出力する。
- `StretchDIBits` は、上から下に並んだ DIB（高さが負）でも元の矩形の Y を下端から数える（`ch - y1` を渡す）。

## 確認のポイント

- ビルドした `.8bf` に従来形式の PiPL（リソース種別 `PIPL`）が入っていること。`JSON_PIPL` だけだと Affinity に表示されない。
