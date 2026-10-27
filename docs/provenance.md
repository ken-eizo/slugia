# 技術と出典

slugiaはEric LengyelのSlugを基礎にした独自のVulkan rendererです。
公式商用Slug SDK全体をlinkしている構成ではありません。

| 部分 | 出典・実装 |
| --- | --- |
| shape / glyphの二次曲線・band・解析的coverage / AA | [Slug公開reference shader](https://github.com/EricLengyel/Slug)。GLSLへ移植し、行またぎ・root計算・保存形式等を改修 |
| atlas生成 | [slughorn](https://github.com/AlphaPixel/slughorn) coreとFreeType bridge。実際にlink |
| font outline / shaping | FreeType / HarfBuzzを実際にlink |
| SVG parse | NanoSVG headerを組み込み |
| window / GPU | GLFW / Vulkan、macOSはMoltenVK |
| 保持描画、cache、View2D、GUI、3D scene / 投影 | slugia側の実装 |
| rounded rectangle等 | 専用の解析的primitive shaderも使用 |

Slug shader code Copyright 2017 by Eric Lengyel.

Vello、Skia、Blend2D、Rive、ThorVG等は設計・試験・比較の研究対象です。
これらのrendererを内部で切り替えたり、その実装をlinkしたりしていません。

依存のrevisionはGit submoduleのgitlinkで固定しています。
slughornは最小coreをvendorし、基となるrevisionとadaptation記録を
[`third_party/slughorn/VENDORED.md`](../third_party/slughorn/VENDORED.md) に保持しています。
NanoSVGはcommit `239e102ec2c691f2902e20ace2ed36ee4a35cfe6` 由来です。

同梱Tigerは[Velloの固定asset](https://github.com/linebender/vello/blob/65338c258f091883c6ab99b4e9b7d2b47caafb4a/assets/Ghostscript_Tiger.svg)。
SHA-256は `ff81e7cb36b08a3b3103c0c268083cf816d2ff969486166337ce692025dd87ee`。
Geistは `examples/assets/fonts/OFL-Geist.txt` の条件で同梱します。
全license・必要creditは [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md) を参照してください。
