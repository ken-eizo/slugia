# SlugUI 宣言型IRとruntime

SlugUIは、slugiaの描画器を特定ホストやデザインツールへ結び付けずに、再利用可能な
宣言型UIへするための中間表現（IR）です。C++ builderと、人が読める`.slugui`をbuild時に
型検査してC++へ変換するAOT compilerの両方が同じIRを構築し、軽量runtimeが既存の
`DrawList`へ直接loweringします。製品runtimeにparserや文字列式VMは入りません。

## 実装済みの境界

公開APIは`include/slugia/slugui.hpp`、実装は`src/slugui.cpp`にあります。

- `Component`: mutableな単体UI。1つの`Element` tree、型付き`PropertyStore`、callback tableを所有
- `ComponentDefinition`: immutableな共有`Element` treeとdefault property schema/valueを所有
- `ComponentInstance`: `ComponentDefinition`を共有し、instance-localなproperty値/callback stateだけを所有
- `RuntimeNodeId { instance, element }`: 共有definition内の同じ`ElementId`をinstance単位で区別するruntime identity
- `Element`: stable ID、名前、layout、visual、interaction、子要素、import metadataを保持
- `Property<T>` / `ValueSource<T>`: 値または型付きproperty slotを参照
- `PropertyStore::clone`: immutable schema/binding graphを共有し、value/revision/dirty queueだけをinstance-localに複製
- `PropertyStore::bind`: 明示した依存propertyのrevisionが変化した場合だけ再評価
- layout: `Absolute`、`Row`、`Column`、`Stack`
- size: auto、論理px、物理px、percent、min/max/preferred、grow、padding、spacing、alignment
- visual: 空container、解析的角丸矩形（fill + stroke）、Slug shape、Slug vector text
- state paint: normal、hovered、pressed、disabled
- input: hover enter/exit、press、release、press-edge activation
- overlay: 親のoverlay状態を子へ伝播し、通常commandの後の`DrawList` overlay batchへ出力
- source metadata: provider、document ID、node ID、node name、import fidelity
- `tools/slugui_compiler.py`: comment対応lexer、parser、型検査、決定的C++ header生成、診断
- `tools/figma-slugui`: Figma selectionを`.slugui`へ変換する開発plugin

このruntimeは別のレンダリングパイプラインを作りません。最終出力は従来と同じ
`DrawList`なので、Slug shape、角丸矩形、vector text、paintは既存の1つのVulkan batch設計を
そのまま利用します。

## 共有definitionとinstance

動的GUIで同じButton/Card/Row定義を多数使う場合、`Element` tree自体は
`ComponentDefinition`に1回だけ保持し、各`ComponentInstance`はdefaultからcloneした
property value/revisionとcallback tableだけを持ちます。`PropertyStore::clone`はschema、
property名表、binding graphを共有するため、instance数に比例してdefinition metadataを複製しません。

`ElementId`はdefinition-localです。同じdefinitionを複数instanceが参照すると同じ
`ElementId`が現れるため、layout/hit-test/interactionのruntime identityは
`RuntimeNodeId { InstanceId, ElementId }`です。`LayoutBox::id`、`UiEvent::target`、
`Runtime::find(ElementId)`は既存コード向けに残し、instance-awareなコードは
`LayoutBox::node`、`UiEvent::targetNode`、`Runtime::find(RuntimeNodeId)`を使用します。
`InstanceId == 0`は従来の単体Component/standalone instance用のunqualified domainです。
同じruntime/sceneで複数instanceを区別する場合はstableな非0 IDを明示します。

AOT生成structは従来のmutable `component`に加えて`makeSharedDefinition()`を生成します。
戻り値の`SharedDefinition`はproperty handleと共有definitionを保持し、
`instantiate(instance_id)`でtreeを複製せずにinstanceを作れます。

複数instanceを同じUI surfaceへ合成する場合は`Scene` / `SceneRuntime`を使います。
`Scene`はstableな非0 `InstanceId`、viewport、device scaleだけをretained保持し、
`SceneRuntime`はinstanceごとの既存`Runtime` cacheを保持します。入力は全instanceへbroadcastせず、
scene painter orderとoverlay orderを使ってtopmost targetを一つだけ選択します。press後はreleaseまで
そのinstanceへpointer captureします。hit-testはScene viewportの128px spatial gridで候補instanceを
絞ってから各Runtimeの既存node spatial indexへ入るため、pointer moveだけで全instanceを走査しません。

`Scene::setViewport()`はviewport/scaleが実際に変化した時だけScene generationを進め、
instance-level spatial indexを再構築します。静止sceneでは同じindexを再利用します。
`Scene::remove()` / `clear()`で構成を変更できます。

`Scene`は`ComponentInstance`を借用します。登録中はinstanceを生存させ、移動しないでください。
`SceneRuntime`は`InstanceVisual`を再帰展開し、子definitionとinstance-local stateを保持します。
C++ builderの`instance(elementId, localId, definition)`を親treeへ追加して使用します。
`localId`は親内で一意の非0値とし、子のruntime IDは`composeInstanceId(parentId, localId)`です。

```cpp
auto host = sui::instance(slugia::hashId("child-host"), 7, childDefinition);
host.layout.width = sui::Length::logical(100);
host.layout.height = sui::Length::logical(40);
auto& visual = std::get<sui::InstanceVisual>(host.visual);
visual.overrides.push_back({childColor.id, slugia::Color::fromRgb8(0x4056a8)});
visual.links.push_back({parentWidth.id, childWidth.id});
parentRoot.add(std::move(host));
// parentRoot / parentDefaultsをComponentDefinitionにしてから:
sui::ComponentInstance parent(parentDefinition, 501);
sui::Scene scene;
scene.add(parent, {0, 0, 800, 600});
sui::SceneRuntime runtime;
runtime.render(scene, window.input(), drawList);
```

上のproperty handleはそれぞれ親・子の`PropertyStore::define`で作成します。
`overrides`は子の値を設定し、`links`は同じ型の親propertyを子へ一方向にコピーします。
毎frame、overrideの後にlinkを適用します。子のcallbackは初回展開後の
`runtime.instance(composeInstanceId(501, 7))->on(...)`へ登録できます。
`find(RuntimeNodeId)`、`hoveredNode` / `activeNode` / `focusedNode`で子を識別します。
definitionの循環、64階層を超える再帰、ID衝突は例外で拒否します。
単体`Runtime`はnested instanceを展開しないため、この用途には`SceneRuntime`が必要です。

## 最小例

```cpp
namespace sui = slugia::slugui;

auto root = sui::column(slugia::hashId("root"));
root.layout.padding = sui::Insets::all(sui::Length::logical(12));
root.layout.spacing = sui::Length::logical(8);

auto button = sui::roundedRectangle(
  slugia::hashId("button"),
  slugia::Paint::solid(slugia::Color::fromRgb8(0x4056a8)),
  slugia::CornerRadii::all(10),
  slugia::CornerSmoothing::all(100));
button.layout.height = sui::Length::logical(42);
button.interaction = {.interactive = true, .enabled = true, .callback = 1};
root.add(std::move(button));

sui::Component component(std::move(root));
auto clicks = component.properties().define<std::int64_t>("clicks", 0);
component.on(1, [&](const sui::UiEvent& event) {
  if (event.type == sui::EventType::Activated)
    component.properties().set(clicks, component.properties().get(clicks) + 1);
});

sui::Runtime runtime;
runtime.render(component, window.input(), drawList, viewport, window.contentScale());
```

`Length::logical`は`deviceScale`を1回掛けてframebuffer pxへ解決します。
`Length::physical`は既にframebuffer pxである値に使い、percentは親のinner sizeに対する
0–100の割合です。角丸半径は論理pxとしてscaleした後に`DrawList::roundedRect`へ渡すため、
矩形のwidth/heightで引き伸ばされません。`CornerSmoothing`は0–100%のままです。

## フレームと遅延

`Runtime::render`の順序は次のとおりです。

1. dirty bindingだけを評価
2. 現在のframebuffer viewportでlayout
3. 現フレームのcursor/button edgeでhit test
4. hover/press/activation callbackを同期実行
5. callbackがpropertyを変更した場合、bindingを評価し同じフレームで再layout
6. normal command、overlay commandの順で既存`DrawList`へlowering

Activationをrelease待ちではなくpress edgeで発火するのは、ボタンと直接操作UIの視覚状態を
OS cursorから余分な1フレーム遅らせないためです。releaseも独立したeventとして取得できます。
layoutはCPUで動作しますが、GPU readbackや追加submitは発生しません。文字列名の検索は
component構築時の`find`用で、render時の参照は整数の`PropertyId`です。

`RuntimeStats`はelement数、visible element数、layout pass数、dispatch event数を返します。layoutはtree walkで処理します。

## layout規則

- `Row` / `Column`: visible childのintrinsic/explicit sizeを測り、余りを正の`grow`比で配分
- childの`position: absolute`はRow/Columnのmeasure・spacing・growから除外し、親content box基準のx/yで配置。
  描画順はflow childと混在してもsource順を維持する
- `crossAlignment`: Start、Center、End、Stretch。childの`alignSelf`があれば上書き
- `mainAlignment`: Start、Center、End、SpaceBetween
- `Stack`: width/heightがautoのchildをinner boundsへ広げ、子を同じ領域へ重ねる
- `Absolute`: childのx/yと測定sizeを使用。x/yのautoは0
- `clipsChildren`: 子のclipを親boundsと交差
- rootのauto width/height: 渡されたviewportを満たす
- overlay: layout参加方法は通常要素と同じで、描画/hit-test順だけを手前へ移す

intrinsic text measurementはcodepointベースです。複雑なscript、bidi、言語別line breakingを完全には再現しません。事前shapingの扱いはapi.mdを参照してください。

## .slugui AOT compiler

```bash
python tools/slugui_compiler.py ui/MyPanel.slugui \
  --output generated/MyPanel.slugui.hpp \
  --namespace my_app::generated
```

`--check`は生成せずparse/type checkだけを行い、`--stdout`はheaderを標準出力へ出します。
`--strip-source-metadata`はstable IDを維持してprovider/source診断情報を生成headerから除外します。
出力は一時fileからatomic replaceされ、同じ入力・namespaceから常に同じbytesを生成します。既存headerと
bytesが同じ場合はmtimeも変更しないため、生成結果が同じ再exportでC++を再compileしません。Exampleの
CMake連携は別の`.stamp`へ「このinput/compiler revisionは検査済み」を記録し、headerのmtimeを
content-stableにしたままPython AOT自体の不要な再実行も避けます。`examples/slugui_demo.slugui`も
この経路でAOT生成され、そのheaderを実際にC++コンパイルしています。

subproject利用では同じ処理をCMake関数から呼べます。

```cmake
add_subdirectory(third_party/slugia)
include(third_party/slugia/cmake/SlugUI.cmake)

slugia_compile_slugui(
  STAMPED
  INPUT "${CMAKE_CURRENT_SOURCE_DIR}/ui/MyPanel.slugui"
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/MyPanel.slugui.hpp"
  NAMESPACE my_app::generated)
```

`STAMPED`は大きいgenerated headerをincludeするtarget向けのincremental-build modeです。
headerが同じbytesならmtimeを維持しつつ`.stamp`だけを更新するため、不要なPython AOTとC++再compileを
両方避けます。通常のcustom command互換が必要なら省略できます。

`CLASS_NAME`またはCLIの`--class-name`は生成C++型だけを固定します。入力component名とstable IDは
変えないため、単一selectionとFigmaの複数selectionを同じhost slotへ差し替えられます。

最小grammar:

```text
component MyPanel {
  property int clicks = 0;
  property string label = "Click";

  Column root {
    padding: 12px;
    spacing: 8px;

    Rectangle action {
      height: 42px;
      fill: linear(#4056a8, #6b8cff);
      fill-hovered: #5470c0;
      stroke: linear(#80d8ff, #ff68b0);
      stroke-width: 2;
      stroke-align: inside;
      radius: 10;
      smoothing: 100%;
      callback: activate;

      Text caption {
        text: $label;
        font-size: 15;
        text-align: center;
      }
    }

    Text rich_label {
      text: "Red blue";
      text-align: center;
      text-align-vertical: bottom;
      text-runs: [
        text-run("Red", "Geist", 16, 700, false, true, false, 0, 1.25, #ff0000),
        text-run(" blue", "Geist", 14, 400, true, false, false, 0.5, 1.4, #0000ff)
      ];
    }
  }
}
```

property型は`bool`、`int`、`float`、`string`、`color`、`paint`、`length`、
`radii`、`smoothing`、`border-widths`です。`$name`は同じ型を要求する`ValueSource` attributeへ直接binding
されます。callback名はcomponent内のstable IDへAOT変換され、生成structの
`callback_<name>`としてC++からhandlerを登録できます。

length suffixは`px`（論理px）、`ppx`（framebuffer物理px）、`%`、`auto`です。
`position: absolute`はRow/Column内のchildだけをflowから外す軽量なitem指定で、通常の
`Absolute` containerとは別です。min/max width/height、grow、align-selfとも併用できます。
paintはcolor literal、`solid`、`linear`、`diamond`、`radial`、`shader`を扱います。
Rectangleの`stroke`にも同じPaintを使用でき、`stroke-width`は論理px、
`stroke-align`は`inside` / `center` / `outside`です。stroke ringは解析シェーダーで
評価され、同一batch内の追加quadになります。`stroke-width: [top, right, bottom, left]`で
4辺を独立指定でき、各辺のInside/Center/Outside量と内外の角丸も個別幅から計算します。
paddingは1値、CSS順の2値`[vertical, horizontal]`、または
`[left, top, right, bottom]`です。radius/smoothingは1値または4角を指定できます。
`text-runs`は静的rangeの文字列と`TextStyle`をAOTで型付き配列へ変換します。引数は
`text, font, size, weight, italic, underline, strikethrough, letter-spacing, line-height, paint`
の順で、各runの文字を連結した結果は`text`と完全一致しなければbuild errorです。
runtimeはrun配列をcopyせず参照し、range別の色・font・weight・size・装飾を同じSlug glyph batchへ
loweringします。

## この版の対応範囲

- runtime hot reloadは非対応。compilerはbuild-time AOTのみ
- Figma exporterはRectangleのstroke paint/width/alignと、SVG由来vectorの
  fill/stroke geometry・paintを実装済み。inside/outsideはFigmaのoutline結果を焼き込む。
  EVENODD windingも保持する。exporterのfidelity分類ではApproximatedを付ける場合がある
- textはrange別family、size、numeric weight、italic、underline/strikethrough、色、
  line height、letter spacing、text caseとblock alignmentを出力し、font fileはアプリ側で明示登録する
- paragraph spacing、paragraph別list nesting、OpenType feature override、hyperlink、
  decorationの個別offset/thickness/color、paragraph bidiはnative再現しない。
  atlasに準備した文字runはHarfBuzzのglyph配置を使用する
- Figma APIからgeometryを取得できないvectorは明示placeholderになる。
  component/instance/variantのsource provenanceと依存変更refreshは実装済み。
  BOOLEAN/TEXT component-property referenceはtyped propertyへlowering済みだが、
  Figma Instanceをmain-definition参照として直接出力するdedup、INSTANCE_SWAP、SLOT差し替え、
  VARIANT変更によるdefinition/tree selection、variables/token modeは未実装
- C++のnested instance合成は`SceneRuntime`で対応する。AOT CLIの`definition` / `Instance`宣言は
  完結した生成経路を持たないため本版の対応外。共有definitionは生成structの
  `makeSharedDefinition`またはC++ builderで作成する
- conditional/repeater、virtualized list、focus traversal、accessibility treeは未実装
- structural tree mutationをcallback中に行う契約は未定義。property変更を使用する
- style inheritance/theme/token tableは未実装
- geometry property変更時もresolved treeの再配置自体は行うが、measure cacheはproperty→element逆依存で
  変更要素と祖先だけ無効化する。10,001要素stressでは単一width変更の実measureは2要素

この一覧が本版の対応範囲です。
