# APIガイド

すべての通常APIは`#include <slugia/slugia.hpp>`から利用できます。座標、size、clip、corner radiusは
framebuffer pixelです。

## PathとVectorAtlas

```cpp
slugia::VectorAtlas atlas;

const auto panelShape = atlas.addPath(
  slugia::Path{}.roundedRect(0, 0, 240, 80, 12, 60));

slugia::Path curve;
curve.moveTo(0, 30).cubicTo(80, -20, 160, 80, 240, 30);

const auto icon = atlas.addPath(
  slugia::Path{}.svgPath("M4 12h16M12 4v16", 24));

slugia::StrokeStyle stroke;
stroke.width = 4;
stroke.join = slugia::LineJoin::Round;
stroke.cap = slugia::LineCap::Butt;
stroke.dashLengths = {12, 7};
const auto curveShape = atlas.addStroke(curve, stroke);

atlas.build();
```

`Path`は`moveTo`、`lineTo`、quadratic/cubic Bézier、rect、rounded rect、circle、ellipse、polygon、
starを連結可能です。`svgPath(data, viewBoxHeight)`はSVGの`M/L/H/V/C/S/Q/T/A/Z`を
absolute/relative双方で読み、arcはcubicへ変換します。`viewBoxHeight > 0`ならY座標とarc sweepを
反転してSVGのY下向き表示を維持し、`0`なら入力座標をそのまま使います。不正dataは
`std::invalid_argument`です。これはatlas構築APIであり、frame loopのSVG parserではありません。
`addPath()`と`addStroke()`の戻り値`0`は登録失敗です。`build()`後のshape/font追加は
`std::logic_error`になります。

`addPath(path, FillRule::EvenOdd)`は輪郭を書き換えず、authored fill ruleをatlas metadataとして保持します。
Vulkan rendererはEric Lengyelのreference shaderと同じE flag (`0x1000`) をglyph metadataへpackし、
Bezier winding coverageの最終段でeven-oddを評価します。`NonZero`が既定です。

```cpp
slugia::Path compound;
compound.rect(0, 0, 100, 100).rect(25, 25, 50, 50);
const auto hole = atlas.addPath(compound, slugia::FillRule::EvenOdd);
```

### Slug reference rendering contract

`third_party/Slug`のEric Lengyel reference shadersを、Slug-compatible rendering pathの規範実装として扱います。
Slug shape/glyphでは元quadratic Bezierをband atlasから直接coverage評価し、固定AA paddingやcoverageの
二値thresholdへ置き換えません。境界拡張はhalf-pixel dynamic dilation、sample座標補正はinverse
Jacobianというreference modelを基準にします。通常UIはorthographic/axis-aligned変換を使います。射影変換はDrawList::projectedと3D UI平面で指定します。

### Strokeの既定値と明示override

通常は`StrokeStyle::cap`だけ指定すれば、pathと全dashの両端へ同じcapを使います。必要な場合だけ
`std::optional` fieldsを指定します。

```cpp
slugia::StrokeStyle s;
s.cap = slugia::LineCap::Butt;              // 通常の全端点
s.startCap = slugia::LineCap::Round;        // path全体の開始だけ
s.endCap = slugia::LineCap::Square;         // path全体の終了だけ
s.dashStartCap = slugia::LineCap::Square;   // 各dash開始
s.dashEndCap = slugia::LineCap::Butt;       // 各dash終了
s.dashCaps.resize(3);
s.dashCaps[2].end = slugia::LineCap::Round; // 3本目だけ
```

優先順位は「`dashCaps[index]` → path端に一致する`startCap/endCap` → dash共通override → `cap`」です。
明示しないfieldはmemoryや宣言を要求せず、従来のcompactな挙動を保ちます。

`width`、dash array、offset、cap、join、start/end taperはatlas geometryです。shapeを登録した後に
`DrawList::stroke(shape, ..., style)`へ別の`width`等を渡してもgeometryは変わらず、`style.paint`だけが
使われます。動的に異なるwidthやdashを選ぶ場合は候補shapeをatlasへ事前登録し、shape IDを切り替えます。

## Paint

`Paint`はfill、stroke、text、analytic rounded rectで共通です。
`Color::fromRgb8()`と`.slugui`のhexはsRGB値です。fragment shaderがlinearへdecodeしてから
sRGB swapchain上でblend/encodeするため、hex値を二重gamma変換しません。

```cpp
const auto solid = slugia::Paint::solid(
  slugia::Color::fromRgb8(0x36d399), 0.8f);

const auto linear = slugia::Paint::gradient(
  slugia::GradientKind::Linear,
  slugia::Color::fromRgb8(0x6d5dfc),
  slugia::Color::fromRgb8(0x28c7fa),
  {0, 0}, {1, 1}, 0.95f);

const auto procedural = slugia::Paint::shader(
  slugia::Color::fromRgb8(0xff4d8d),
  slugia::Color::fromRgb8(0xffd166), 0.6f);
```

`GradientKind`はSolid、Linear、Diamond、Radial、Shaderです。origin/targetはshape内の正規化paint座標です。
`opacity`はColor alphaに加えてpaint全体へ適用されます。現在の`Shader`は内蔵procedural分岐とfloat
parameterであり、利用者の任意SPIR-V moduleを登録するAPIではありません。

## DrawList

```cpp
draw.clear();
draw.setClip({0, 0, framebuffer.x, framebuffer.y});
draw.fill(panelShape, {20, 20, 320, 100}, linear);
draw.stroke(curveShape, {20, 150, 320, 80}, procedural);
draw.cubicBezier({20, 180}, {100, 120}, {220, 240}, {320, 180}, stroke);

draw.beginOverlay();
draw.roundedRect({300, 60, 180, 120}, 12, solid, 100);
draw.endOverlay();
```

`DrawList`は宣言順を保ちます。通常commandの後に全overlay commandを出すため、dropdownやtooltipを
panelより前面にできます。`clear()`はcommandsとoverlay modeをresetしますが、vectorのcapacityは再利用します。

`cubicBezier()`はatlas shapeと異なり、4点をframebuffer座標のまま保持します。rendererがframe構築時に
screen-space誤差に応じて分割し、各区間を解析的AA strokeとしてGPUへ送ります。control pointが毎frame変わる
editor handle等に向きます。静的curveは`Path::cubicTo()`をatlasへ一度登録する方がCPU処理を省けます。

### VulkanInterop（advanced / opt-in）

通常のUI描画では不要です。既存Vulkan compute/transferとzero-copy連携する場合だけ
`#include <slugia/vulkan_interop.hpp>`を追加し、`renderer.vulkanInterop()`から取得します。

```cpp
slugia::VulkanInterop& interop = renderer.vulkanInterop();
const auto device = interop.deviceContext();
const auto image = interop.registerRgba32fBuffer(buffer, offset, byteRange);
draw.externalImage(image, bounds, width, height);
```

外部bufferは`deviceContext().device`から作成した`VkBuffer`でなければなりません。pixel storageは
float32のA,R,G,Bをpixelごとに4word、隙間なく並べます。bufferの所有権はcallerに残り、登録中は
有効に保ちます。`unregisterExternalImage()`は現在安全性優先でdevice idleを待ってdescriptorを破棄します。

CPU snapshot用途にはrenderer所有bufferも利用できます。

```cpp
const auto image = interop.createOwnedRgba32fImage(width, height);
interop.updateOwnedRgba32fImage(image, argbWords);
draw.externalImage(image, bounds, width, height);
```

`setFrameRecorder()`はslugiaの同じcommand bufferへ、UI render pass直前のVulkan commandを記録する
高度なhookです。callback内でsubmit/wait/throwしたりcommand bufferを保持してはいけません。
`completedSubmissionSerial()`はblockingせず完了済みsubmissionの上限を返します。Interop IDとcallbackは
作成元`VulkanRenderer`の寿命内だけ有効です。

### 絶対pixelの角丸

```cpp
draw.roundedRect({20, 20, 360, 72}, 12, paint, 80);

draw.roundedRect(
  {20, 110, 360, 72},
  {.topLeft = 4, .topRight = 12, .bottomRight = 20, .bottomLeft = 28},
  paint,
  {.topLeftPercent = 0, .topRightPercent = 25,
   .bottomRightPercent = 50, .bottomLeftPercent = 100},
  {.paint = borderPaint, .width = 2, .align = slugia::StrokeAlign::Inside});
```

矩形のwidth/heightを確定した後に各radiusを適用します。互いのradiusが物理的に収まらない場合だけ、
CSS互換の比率で全radiusを縮小します。通常の範囲では指定pixel値を維持します。
`BorderStyle::align`は`Inside` / `Center` / `Outside`です。外側量をそれぞれ0 / width÷2 /
widthとして解析ringのboundsと半径へ適用し、fillの後にborderを描くためinside strokeも塗りに
隠れません。直線と角丸は同じ連続距離式でAA評価されます。

4辺を独立させる場合は均一値`width`ではなく`individualWidths`を設定します。順序はFigmaと同じ
top/right/bottom/leftです。

```cpp
slugia::BorderStyle border;
border.paint = borderPaint;
border.align = slugia::StrokeAlign::Outside;
border.individualWidths = slugia::BorderWidths{1, 2, 3, 4};
```

## Vector text

```cpp
const auto needed = slugia::decodeUtf8("Vector text");
atlas.loadFont(
  "examples/assets/fonts/Geist[wght].ttf",
  {.family = "Geist", .weight = 400, .italic = false},
  needed);
atlas.loadFont(
  "examples/assets/fonts/Geist[wght].ttf",
  {.family = "Geist", .weight = 700, .italic = false},
  needed);

slugia::TextStyle style;
style.fontName = "Geist";
style.weight = 700;
style.size = 18;
style.italic = false;
style.underline = true;
style.lineHeight = 1.4f;
style.letterSpacing = 0.5f;
style.indent = 12;
style.align = slugia::HorizontalAlign::Left;
style.listMarker = slugia::ListMarker::Bullet;
style.paint = paint;

draw.text("Vector text", {20, 20, 480, 120}, style);
```

fontはatlas.buildより前に登録します。file / memoryのほか、`findSystemFont` / `loadSystemFont` による
family・weight・italic指定の明示的なsystem font解決があります。存在しないfontやglyphの完全再現は保証しません。
`FontFace`を省略したoverloadはFreeTypeのfamily/style名を使います。
同一familyを複数登録すると、描画時にweight/italic一致を優先し、なければ最も近いweightを選びます。
variable fontでは`weight`をFreeTypeの`wght` axisへ渡します。空のcodepoint listはASCII 95 glyph、
日本語等は対応するfont fileと必要codepointを明示します。

Left/Center/Right、line height、letter spacing、indent、簡易bullet/number marker、weight/italic face、
synthetic bold、underline/strikethroughを扱います。`Justify` は語間展開を行いません。
`prepareText(text, family, weight, italic)` をatlas.build前に呼ぶとHarfBuzzで必要glyphを登録します。
`shapeText` は準備済みrunを返し、rendererはglyph ID / advance / offsetを再利用します。
未準備の動的textはcodepoint layoutへfallbackします。paragraph全体のbidiとlocale line breakは提供しません。
`InputWriter` はIME composition / selection / commit / cancelを保持し、native candidate windowはhost側です。

### 4種類のtext宣言

- `text(std::string, ...)`: commandが文字列を所有。毎フレーム変化するlabel向け
- `textStatic(std::string_view, ...)`: copyを省くが、参照bytesを`draw()`終了まで生存させる
- `textRunsStatic(std::span<const TextRun>, ...)`: range別styleをcopyせず描画するAOT/static UI向け
- `createRetainedText()` + `retainedText()`: layout/uploadを一度だけ行い、zoom/panはtransformのみ

```cpp
std::array<slugia::TextRun, 2> runs{{
  {"red ", {.fontName = "Geist", .size = 16, .weight = 700,
            .paint = slugia::Paint::solid(slugia::Color::fromRgb8(0xff0000))}},
  {"blue", {.fontName = "Geist", .size = 14, .weight = 400,
             .italic = true,
             .paint = slugia::Paint::solid(slugia::Color::fromRgb8(0x0000ff))}},
}};
draw.textRunsStatic(runs, {20, 20, 480, 40}, style);

const auto document = renderer.createRetainedText(longText, {0, 0, 900, 5000}, style);

draw.setClip(viewport);
draw.retainedText(document, pan, zoom);
```

保持IDは作成元renderer/surfaceの寿命内だけ有効です。`updateRetainedText()` / `destroyRetainedText()`があり、
同一内容は0-byte、部分変更はshared device-local arenaへのdirty range uploadだけを行います。
`textRunsStatic`が参照する配列と各`TextRun::text`は`draw()`完了まで生存させます。

## Input

`Window::pollEvents()`ごとにedge stateを更新します。

```cpp
const auto& input = window.input();
const auto cursor = input.cursorPosition();
const auto delta = input.cursorDelta();

if (input.mouse(slugia::MouseButton::Left).pressed) { /* 押したframe */ }
if (input.mouse(slugia::MouseButton::Left).down)    { /* 保持中 */ }
if (input.mouse(slugia::MouseButton::Left).released){ /* 離したframe */ }

if (input.scroll().started) { /* gesture開始 */ }
if (input.scroll().active)  { /* 最後のeventから120 ms以内 */ }
if (input.scroll().ended)   { /* timeoutしたframe */ }
```

core UIの物理key状態にはplatform非依存`slugia::Key`を使います。Unicode committed textは`textInput()`、
IME未確定文字列は`composition()`で取得できます。GLFW backendはnative codeを`Key`へ変換します。
raw mouse motionはplatformが対応する場合に`setRawMouseMotion(true)`でcursor captureと共に有効化します。

外部hostは`InputWriter`で同じ`InputState`を作れます。frameにつき
`beginFrame()` → host event変換 → `finishFrame(nowSeconds)`の順に呼びます。
`cursor()`へ渡す座標はframebuffer pixelです。`composition()` / `commitComposition()` / `cancelComposition()`でIMEを渡し、
`focusLost()`は全down stateとactive compositionを解除するためcapture/focus喪失時に必ず呼びます。

## 軽量layoutとtext edit

`Insets`、`inset/outset`、`LinearLayout`、`gridColumns`は保持treeを作らない決定的な
rectangle計算です。`gridColumns`の正値は固定pixel、負値の絶対値は残余幅のweightです。

```cpp
slugia::LinearLayout rows(bounds, slugia::Axis::Vertical, 8);
const auto toolbar = rows.take(36);
const auto body = rows.remaining();

const std::array<float, 3> columns{180, -1, -2};
const auto cells = slugia::gridColumns(body, columns, 8);
```

`TextEditState`はUTF-8 codepoint境界を壊さず、caret/anchor、選択削除、insert、
backspace/delete、left/right/home/end、最大128 snapshotのundo / redoを扱います。
`beginTransaction` / `endTransaction` で複数変更を一つの履歴へまとめ、`clearHistory` で解除できます。
grapheme clusterは扱いません。clipboardはUiContextのcallback、IME stateはInputStateへ接続します。
`UiContext::textField`はwidget IDごとにこの状態を保持し、Shift選択、
Ctrl/Cmd+A、caretと選択範囲を描画します。

## UiContext

`UiContext`はimmediate/declarative UIです。modelはapplicationが所有します。

```cpp
slugia::UiSkin skin;
skin.rectangle = rectangleShape;
skin.circle = circleShape;
skin.check = checkShape;
skin.cornerRadius = 8;
skin.continuousCornersPercent = 100;
slugia::UiContext ui(skin);

ui.beginFrame(window.input(), draw);
if (ui.button(slugia::hashId("save"), "Save", {20, 20, 120, 34})) save();
ui.slider(slugia::hashId("opacity"), "Opacity", {20, 64, 260, 34}, opacity, 0, 1);
ui.textField(slugia::hashId("name"), {20, 108, 260, 34}, name, "Name");
ui.endFrame();
```

各IDは同一frame内で一意かつframe間で安定させます。labelをIDとして暗黙利用しません。同じ表示文字列の
buttonも別IDなら共存できます。treeの内部IDは親とsibling indexから作るため同名nodeが衝突しません。

widgetの`bool`戻り値は、button/radioではpress edge、model参照を受けるcontrolでは値が変化したframeです。
視覚反応もpress frameで宣言されます。`interaction()`を使えばhovered、pressed、released、held、cursor、
deltaをcustom widgetで取得できます。

実装済みwidget:

- button、spin button、slider、list box、checkbox、combo/dropdown、radio
- UTF-8 text field / text area、toggle switch、縦横scrollbar、tooltip
- tree view、grid view

combo/dropdownとtooltipはoverlay commandへ送られます。同時に開けるcombo ownerは一つで、menu領域は下の
通常widgetのhit testを遮断します。

```cpp
slugia::Vec2 scroll{}; // frame間で保持
ui.textArea(slugia::hashId("editor"), {20, 160, 600, 300}, source, scroll, "Text",
    {.showLineNumbers = true, .showScrollIndicators = true, .monospaced = true});
```

text areaは複数行編集・scroll・selection・IME preedit表示を扱います。monospaced modeはWindowsでConsolas、
macOSでMenlo、LinuxでDejaVu Sans Monoを指定するので、使用fontと必要glyphをatlasへ登録してください。
`setClipboardCallbacks` でread / writeを登録するとcopy / cut / pasteを使えます。
`focus` / `focusText` / `resetTextEdit` は外部からfocus・caret・編集状態を設定します。

## SlugUI IR / AOT component

`UiContext`は即時widget API、`slugia::slugui`は再利用可能なcomponent treeです。後者はC++
builderまたは`.slugui` AOT compilerで構築し、どちらも同じ`Runtime::render`から
`DrawList`へloweringします。

```cpp
slugia::example::SlugUiDemoGenerated generated(atlas); // atlas.buildより前
generated.component.on(
  slugia::example::SlugUiDemoGenerated::callback_increment,
  [&](const slugia::slugui::UiEvent& event) {
    if (event.type == slugia::slugui::EventType::Activated) {
      auto& properties = generated.component.properties();
      properties.set(generated.clicks, properties.get(generated.clicks) + 1);
    }
  });

slugia::slugui::Runtime runtime;
runtime.render(generated.component, window.input(), draw, viewport, window.contentScale());
```

生成structは`component`、型付きproperty handle、stable callback IDを公開します。property名検索、
DSL parse、式評価はframe loopで行いません。

同じcomponent treeを多数使う場合は、生成structの共有definition APIを使います。

```cpp
auto shared = slugia::example::SlugUiDemoGenerated::makeSharedDefinition(atlas);
auto first = shared.instantiate(1001);
auto second = shared.instantiate(1002);

first.properties().set(shared.counter_label, std::string("First"));
second.properties().set(shared.counter_label, std::string("Second"));

first.on(slugia::example::SlugUiDemoGenerated::callback_increment,
  [&](const slugia::slugui::UiEvent& event) {
    if (event.type == slugia::slugui::EventType::Activated) {
      // event.target is definition-local。instanceを含むidentityが必要ならtargetNodeを使う。
      const auto node = event.targetNode;
      (void)node;
    }
  });

slugia::slugui::Scene scene;
scene.add(first, firstViewport, window.contentScale());
scene.add(second, secondViewport, window.contentScale());

slugia::slugui::SceneRuntime sceneRuntime;
sceneRuntime.render(scene, window.input(), draw);

// 配置だけが変わった場合。値が同じならgeneration/indexは更新されない。
scene.setViewport(1002, movedSecondViewport, window.contentScale());
```

`SharedDefinition`は1つのimmutable `ComponentDefinition`とproperty handleを保持し、
各`ComponentInstance`はproperty value/revisionとcallback stateだけを独立所有します。
`RuntimeNodeId { instance, element }`により、共有tree上の同じElementをinstance単位で区別できます。
`SceneRuntime`は重なったinstanceのpainter/overlay順を一度解決して入力を1つのtargetへ配送し、
press後はreleaseまでpointer captureを維持します。
詳細なgrammar、layout規則、制限は[SlugUI宣言型IR](slugui.md)を参照してください。

## Animation

```cpp
auto opacityTween = slugia::tween(0.0f, 1.0f, 180.0f, slugia::Easing::EaseOut);
const float opacity = opacityTween.update(deltaMilliseconds);
```

組み込み補間は`float`と`Color`です。任意型は`Tween<T>`へinterpolatorを渡します。時間単位はmsで、
Linear、EaseIn、EaseOut、EaseInOut、SmoothStep、Springを選べます。animation自体はframe schedulerを
所有しないため、host loopが`deltaMs`を供給し、必要な間だけ再描画します。
