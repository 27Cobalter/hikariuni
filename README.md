# Hikariuni

写真の中の光源を見つけて、クロスフィルターのような**光芒（光の筋）**を描く、Affinity 向けのフィルタープラグイン。

名前は **光（hikari）＋ ウニ（uni）**。光源から伸びる光芒が、ウニのトゲのように見えることから。

> [!NOTE]
> 開発中。今の `.8bf` は Affinity への読み込み確認用で、**色を反転するだけ**（Affinity での読み込みと動作は確認済み）。
> 光芒の処理はこれから実装する。

## 特長（予定）

- 明るさだけでなく**周囲とのコントラスト**で光源を判定し、星や水面のきらめきなど、点状の光にだけ光芒を描く。
- 花びらや雲などの大きな明るい物体、物の縁などの細長い部分は自動で除外する。
- 光芒の本数・角度・長さ・太さ・虹色のにじみ・ハローを調整できる。
- RGB / グレースケール、8bit / 16bit に対応。

## 動作環境

- Windows 10 / 11（x64）
- Photoshop 互換プラグインに対応した Affinity

## ビルド

### 必要なもの

- Visual Studio 2026（「C++ によるデスクトップ開発」）
- Adobe Photoshop Plug-in SDK
  - リポジトリには含めていない。Adobe のライセンスに同意したうえで各自入手すること。

### 手順

1. SDK の `pluginsdk` フォルダを環境変数 `PHOTOSHOP_SDK_PATH` に設定する。
   ```bat
   setx PHOTOSHOP_SDK_PATH "C:\path\to\photoshop-cpp-sdk\pluginsdk"
   ```
2. 次のどちらかでビルドする。
   - Visual Studio 2026 で `src/hikariuni.slnx` を開き、構成を **Release / x64** にしてビルド。
   - 「Developer Command Prompt for VS 2026」から MSBuild でビルド。
     ```bat
     msbuild src\hikariuni.slnx -p:Configuration=Release -p:Platform=x64
     ```
3. `src/x64/Release/hikariuni.8bf` ができる。

### フォルダ構成

```
src/
  hikariuni.slnx      ソリューション
  hikariuni/          プラグイン本体（.vcxproj、ソース、PiPL の定義）
docs/
  spec.md             仕様
```

## Affinity への導入

1. `hikariuni.8bf` を好きなフォルダに置く。
2. Affinity の設定で「Photoshop プラグイン」を開く。
   - **Plugin Search Folders** に、`.8bf` を置いたフォルダを追加する。
   - **Allow Unknown Plugins to be used** をオンにする。自作のプラグインは Affinity の動作確認リストに入っていないので、オフのままだと読み込まれない。
3. Affinity を再起動すると、フィルターメニューのプラグインの中に **Hikariuni** が出る。

## ドキュメント

- [仕様](docs/spec.md)

## ライセンス

[MIT License](LICENSE)

Photoshop Plug-in SDK は Adobe のライセンスに従う。
Adobe と Photoshop は Adobe Inc. の商標または登録商標。その他の製品名も、それぞれの会社の商標または登録商標。
