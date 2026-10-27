# slugia

C++20 / Vulkanのベクター描画ライブラリです。2D図形、文字、SVG、保持描画、GUI、任意の3D拡張を含みます。

図形・文字の曲線被覆計算は[Eric LengyelのSlug](https://github.com/EricLengyel/Slug)に基づき、
公開シェーダーを移植・改修しています。atlas生成に[slughorn](https://github.com/AlphaPixel/slughorn)、
文字にFreeType / HarfBuzz、SVG解析にNanoSVGを使います。
Slug shader code Copyright 2017 by Eric Lengyel.

## ビルド

CMake 3.24以降、C++20コンパイラー、Vulkan SDK（glslc）、Python 3.10以降が必要です。
Windows、Linux、macOS（MoltenVK）を対象にしています。

```sh
git clone --recurse-submodules https://github.com/ken-eizo/slugia.git
cd slugia
cmake -S . -B build
cmake --build build --config Release
```

サンプルは `slugia-example`、`slugia-vector`、`slugia-3d` です。
Windowsでは `build/Release/`、単一構成ビルドでは `build/` に生成します。
ライブラリのCMakeターゲットは `slugia::slugia`、公開ヘッダーは `<slugia/slugia.hpp>` です。

## ドキュメント

- [ビルド・組み込み・実行](docs/build.md)
- [2D・文字・入力・ウィジェット・アニメーション](docs/api.md)
- [SVG・保持描画・ズーム・設定・計測](docs/rendering.md)
- [SlugUIとAOTコンパイラー](docs/slugui.md)
- [Figma取り込み](docs/figma.md)
- [3D・モデル・射影UI](docs/3d.md)、[Slug3D形式・Blender出力](docs/format.md)
- [ネイティブホストとVulkan連携](docs/embedding.md)
- [公開API一覧](docs/reference.md)、[検証と対応範囲](docs/validation.md)
- [技術の出典](docs/provenance.md)

本体はMIT Licenseです。依存コードと同梱データの条件は [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) を参照してください。
