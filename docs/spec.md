# Hikariuni 仕様

決まった仕様をまとめる。仕様を変えたら、同じ変更の中でこのファイルも更新する。

## 概要

| 項目 | 内容 |
|---|---|
| 名前 | Hikariuni（光 hikari ＋ ウニ uni。光芒がウニのトゲのように広がることから） |
| 種類 | Photoshop 互換フィルタープラグイン（`.8bf`） |
| 動作確認するホスト | Affinity（Canva 版、Windows） |
| プラットフォーム | Windows x64 のみ |

### 命名の方針

- 既存製品の名前や、それを連想させる名前を、製品名・メニュー名・UI に使わない。
- 説明文では「光芒」「クロスフィルター」など一般的な言葉を使う。

## フォルダ構成

```
src/
  hikariuni.slnx          ソリューション
  hikariuni/              プラグイン本体のプロジェクト
    hikariuni.vcxproj
    hikariuni.cpp
    hikariuni.r           PiPL の定義
    hikariuni.rc
docs/spec.md              この仕様
```

- プロジェクトは `src/<プロジェクト名>/` に 1 フォルダずつ置き、`src/hikariuni.slnx` に追加する（テスト用プロジェクトなども同じ形で増やす）。

## ビルド

- Visual Studio 2026（MSVC、PlatformToolset `v145`）の `.slnx` ＋ `.vcxproj`。構成は Debug / Release、プラットフォームは x64 のみ。
  - コマンドラインでは VS 2026 の MSBuild: `MSBuild src\hikariuni.slnx -p:Configuration=Release -p:Platform=x64`
- Photoshop Plug-in SDK はリポジトリに含めない（Adobe のライセンスのため）。
  - 環境変数 `PHOTOSHOP_SDK_PATH` に SDK の `pluginsdk` フォルダを指定する。未設定ならビルドをエラーで止める。
  - 使うのは `photoshopapi/photoshop`、`photoshopapi/pica_sp`、`samplecode/common/includes` のヘッダーと `samplecode/resources/Cnvtpipl.exe` だけ。
  - SDK ヘッダーが Windows 用の分岐を選ぶよう `WIN32=1` を定義する。
- C ランタイムは静的リンク（`/MT`、Debug は `/MTd`）。配布先に VC++ ランタイムを要求しない。
- ソースは UTF-8（`/utf-8`）、警告レベル `/W4`。SDK ヘッダーは外部ヘッダー扱いで警告を出さない。
- 出力は `src/x64/<構成>/hikariuni.8bf`。
- 整形: C++ は `.clang-format`。

## PiPL

- **従来のバイナリ形式**（リソース種別 `PIPL`、ID 16000）を使う。
  - 新しい SDK が使う JSON 形式（リソース `JSON_PIPL`）は Affinity が読めず、プラグインとして認識されない。
- 生成の流れ（`hikariuni.vcxproj` のカスタムビルド）: `hikariuni.r` → `cl /EP` → `Cnvtpipl.exe` → 中間フォルダの `hikariuni.pipl` → `hikariuni.rc` で `#include`。

| プロパティ | 値 |
|---|---|
| Kind | Filter |
| Name / Category | `Hikariuni` / `Hikariuni` |
| Version | 4.0（latestFilterVersion） |
| エントリーポイント | `CodeWin64X86 { "PluginMain" }` |
| 対応モード | RGB、グレースケール（8bit / 16bit） |
| PlugInMaxSize | 2,000,000 × 2,000,000 |
| FilterCaseInfo | 全ケースで straight data 入出力、copySourceToDestination |

## フィルター処理（現状）

- 対応: RGB / グレースケール、8bit / 16bit。16bit は Photoshop の仕様どおり 0〜32768。それ以外のビット深度は `filterBadMode` を返す。
- ホストが `bigDocumentData` を持っていれば 32bit 座標を使う。
- `filterSelectorStart` で画像全体・全プレーンを一度に要求する（タイル分割なし）。
- **現在の処理は Affinity への読み込み確認用の色反転**（色プレーンのみ。透明度は保持）。光芒の処理に置き換える予定。
  - Affinity（Canva 版、Windows）で、読み込みと色反転ができることを確認済み（2026-10-15）。
- About はメッセージボックスを表示する。

## 光芒アルゴリズム（予定）

C++ にはまだ実装していない。

### 光源の検出

1. 長辺 2000px に縮小して検出する。
2. 輝度 `L = 0.2126R + 0.7152G + 0.0722B`。
3. 局所コントラスト `C = L − 周囲平均`。周囲平均は半径 `R = max(3, round(最大サイズ × 1.5))` の箱平均（積分画像で計算）。`L < 明るさ` の画素は `C = 0`。
4. `C ≥ コントラスト` の画素を 4 近傍で連結し、ひとつの光源候補にする。
5. 次の候補は除外する。
   - 等価直径 `2√(面積/π)` が最大サイズを超える（花・雲・天の川の塊など）。
   - 縦横比が 3 を超える（共分散行列の固有値比が 9 を超える。物の縁や線）。
   - 周囲（半径 R の正方形）に、候補の平均輝度 × 0.85 以上の画素が 10% を超えてある（花びらの先端など、大きな物体の一部）。
6. エネルギー = 候補内の `C` の合計。エネルギーの降順に並べる。
7. 色 = 候補画素の平均色を、最大チャンネルが 255 になるよう正規化したもの。

### 配置

- エネルギー順に、採用済みの光源から最小間隔以上離れているものだけを、最大個数まで採用する。
- 重み `wt = √(エネルギー / 最大エネルギー)`。

### 描画（加算合成）

- 単位 `u = 短辺 / 1000` px。長さ・太さ・ハロー・最小間隔はこの単位で指定する。
- 不透明度 `a = min(1, 強さ × (0.3 + 0.7wt))`。
- ハロー: 半径 `ハロー × u × wt` の放射グラデーション（中心 `0.6a` → 0）。
- 光芒: 角度から等間隔に本数ぶん。中心の幅 `太さ × u × (0.3 + 0.7wt)`、先端で幅 0 の三角形。長さ方向に `a` → `0.4a`（25% 地点）→ 0。
- 虹色（分散）: R・G・B を別々に描き、長さを R は `×(1 + 0.2 × 分散)`、G は `×1`、B は `×(1 − 0.2 × 分散)` にする。分散 0 なら元の色と同じになる。

### パラメーター

| 名前 | キー | 範囲 | 既定値 |
|---|---|---|---|
| 明るさ | thr | 0〜255 | 160 |
| コントラスト | contrast | 5〜200 | 60 |
| 最大サイズ | size | 1〜50 | 8 |
| 最大個数 | max | 1〜300 | 60 |
| 最小間隔 | gap | 0〜200 | 60 |
| 光芒の本数 | rays | 2〜16 | 4 |
| 角度 | angle | 0〜180° | 45 |
| 長さ | len | 0〜600 | 200 |
| 太さ | width | 0.5〜20 | 3 |
| 強さ | power | 0〜2 | 1 |
| 虹色（分散） | disp | 0〜1 | 0.3 |
| ハロー | glow | 0〜200 | 30 |
