# 公開API

通常は `<slugia/slugia.hpp>`、CMakeは `slugia::slugia` を使います。
下記headerが全公開宣言、fieldの型、既定値とoverloadの定義です。

| Header | 内容 | 使用説明 |
| --- | --- | --- |
| [types.hpp](../include/slugia/types.hpp) | Vec2/3、Rect、Color、Paint、stroke / border / text style、2D/3D変換、ID | [2D API](api.md)、[3D](3d.md) |
| [vector_atlas.hpp](../include/slugia/vector_atlas.hpp) | Path、atlas、font登録・検索、prepared shaping、metrics、UTF-8 decode | [2D・文字](api.md) |
| [vector_path.hpp](../include/slugia/vector_path.hpp) | 二次曲線列、cubic近似、ellipse / arc / rounded rect生成、bounds | [動的描画](rendering.md) |
| [draw_list.hpp](../include/slugia/draw_list.hpp) | shape / text / primitive / dynamic / retained / projected / image command、clip、opacity、overlay | [API](api.md)、[保持と射影](rendering.md) |
| [svg.hpp](../include/slugia/svg.hpp) | SVG parse / load、登録、draw list、入力上限 | [SVG](rendering.md) |
| [vulkan_renderer.hpp](../include/slugia/vulkan_renderer.hpp) | config / preset、draw、retained resource、readback、stats、GPU serial | [描画設定](rendering.md) |
| [render_device.hpp](../include/slugia/render_device.hpp) | RenderDevice factory、move可能なRenderSurface | [埋め込み](embedding.md) |
| [platform_surface.hpp](../include/slugia/platform_surface.hpp) | 外部windowのsurface契約 | [埋め込み](embedding.md) |
| [win32_surface.hpp](../include/slugia/win32_surface.hpp) | 借用HWND。Windowsのみ、個別include | [Windows](embedding.md) |
| [vulkan_interop.hpp](../include/slugia/vulkan_interop.hpp) | device / queue context、RGBA32F buffer、recorder、serial。個別include | [Vulkan連携](embedding.md) |
| [window.hpp](../include/slugia/window.hpp) | GLFW window、resize / refresh / drop、raw cursor、input snapshot | [起動](build.md)、[入力](api.md) |
| [input.hpp](../include/slugia/input.hpp) | Key / MouseButton、InputState / InputWriter、composition、scroll、focus | [入力](api.md)、[host接続](embedding.md) |
| [layout.hpp](../include/slugia/layout.hpp) | Insets、inset / outset、LinearLayout、fixed / flex gridColumns | [layout](api.md) |
| [text_edit.hpp](../include/slugia/text_edit.hpp) | UTF-8 byte境界、caret / selection、編集・移動・composition state | [文字編集](api.md) |
| [ui.hpp](../include/slugia/ui.hpp) | Skin / interaction、button / slider / spin / check / radio / toggle、list / combo / dropdown、text field / area、scrollbar / tooltip / tree / grid、clipboard | [UiContext](api.md) |
| [slugui.hpp](../include/slugia/slugui.hpp) | typed property / binding、element、component definition / instance、runtime、source fidelity / metadata | [SlugUI](slugui.md)、[Figma](figma.md) |
| [animation.hpp](../include/slugia/animation.hpp) | Easing、Tween、float / Color tween helper | [animation](api.md) |
| [vector3d.hpp](../include/slugia/vector3d.hpp) | mesh / curve / camera / scene、生成・準備・投影・遮蔽・pick、OBJ / Slug3D | [3D](3d.md)、[形式](format.md) |
| [slugia.hpp](../include/slugia/slugia.hpp) | umbrella header。3D / GLFWはbuild設定に従う | [ビルド](build.md) |

## 補助API

- `VectorAtlas::native()` はslughorn atlasの読み取り用bridge。通常の描画で利用する必要はありません。
- `createRetained*` / `updateRetained*` / `destroyRetained*` はrendererとRenderSurface双方にあります。
- `decodeUtf8` とUTF-8 boundary helperはbyte indexを扱い、grapheme境界のAPIではありません。
- `gridColumns` は非負値を固定px、負値の絶対値をflex比とします。`LinearLayout::take` は残り範囲から順に切り出します。
- `Tween<T>` はduration ms、easing、補間関数を保持します。`update(deltaMs)`、`restart`、`value`、`running` を使います。
  EasingはLinear / EaseIn / EaseOut / EaseInOut / SmoothStep / Springです。
- Curve helperの許容誤差はpixel基準です。`appendCubicQuadratics` 等のbool helperはfalseで失敗を通知し、追加前へ戻します。
  Path / importerの不正値・入力上限は例外です。

実際の組合せは `examples/kitchen_sink.cpp`、`vector_benchmark.cpp`、`vector3d_demo.cpp`、
`slugui_demo.slugui` と対応する `tests/` にあります。
