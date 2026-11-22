# FigmaからSlugUIへの取り込み

目的は、Figmaの見た目を単なる画像として貼ることではなく、編集可能なSlugUI component、
layout、vector resourceへ変換します。`tools/figma-slugui`に、現在の
selectionを`.slugui`へ変換するnetwork権限なしの開発pluginを実装しています。出力は
`tools/slugui_compiler.py`が型検査し、runtime parserを使わないC++ headerへAOT変換します。

## 採用する構成

1. Figma desktopのPlugins > Development > Import plugin from manifestを開く。
2. `tools/figma-slugui/manifest.json` を選ぶ。
3. nodeを選択してSlugUI Exporterを実行し、出力と警告を確認して `.slugui` をdownloadする。
4. `python tools/slugui_compiler.py Panel.slugui --output generated/Panel.hpp --namespace app::generated` で生成する。
5. 生成headerをincludeし、atlas登録をbuild前に行い、componentをRuntimeへ渡す。

manifestのIDは開発用です。Figmaのplugin配布機能を使う場合はFigmaが割り当てたIDを指定します。

```text
Figma document / selected component
             |
       Figma exporter plugin
             |
          versioned .slugui source
             |
 slugui_compiler.py (parse / type check / SVG path AOT)
             |
 SlugUI IR -> generated C++ descriptor + atlas registration
             |
       SlugUI runtime -> DrawList -> Vulkan
```

製品runtimeはFigma API、parser、network accessを含みません。plugin/compilerは開発時の
toolchainであり、生成物だけをアプリケーションへ組み込みます。これにより編集時の情報量と、
実行時の小ささ・決定性を両立します。

Figma pluginは公式Plugin APIで選択中のcomponent/frameを読み、plugin UIで変換結果と警告を
提示します。実装時は公式の
[FrameNode / auto layout](https://developers.figma.com/docs/plugins/api/FrameNode/)、
[variables](https://developers.figma.com/docs/plugins/working-with-variables/)、
[components](https://developers.figma.com/docs/plugins/api/ComponentNode/)、
[vector network](https://developers.figma.com/docs/plugins/api/VectorNetwork/)、
[styled text segments](https://developers.figma.com/docs/plugins/api/properties/TextNode-getstyledtextsegments/)
を基準にします。

## semantic-first mapping

| Figma | SlugUI / slugia | fidelity |
|---|---|---|
| Frame auto layout horizontal | `Row` | Native |
| Frame auto layout vertical | `Column` | Native |
| Frame without auto layout | `Absolute`または`Stack` | Native / Approximated |
| padding / item spacing / sizing mode | padding / spacing / auto・fixed・grow / min-max | Native |
| auto-layout内のabsolute child | `position: absolute` + x/y。Row/Column flowから除外 | Native |
| Rectangle + independent corner radii | `RoundedRectangleVisual` + `CornerRadii` | Native |
| 回転・非等方スケール・せん断（Figma affine） | `transform: [a,b,c,d]` + parent-local x/y、Runtimeの親子行列合成とVulkan投影描画 | Native（affine） |
| Rectangle stroke paint / weight / align | `StrokeVisual` + 解析stroke ring | Native |
| Rectangle side-specific stroke weights | `[top, right, bottom, left]` + 非対称解析ring | Native |
| corner smoothing | `CornerSmoothing` 0–100% | Native |
| solid / 2-stop linear / 3–256-stop linear | 共通`Paint`。multi-stopは共有stop tableをGPUバッファへ保持 | Native |
| Rectangle / Frameの複数fill | `fill-layers: [paint, ...]`で元の順序をすべて保持。Normal alpha blendをGPU合成 | Native（Normal合成のみ） |
| opacity | `Paint::opacity`またはelement opacity拡張 | Native / Approximated |
| vector fill/center stroke geometry | node相対`path-data` -> `Path` -> `VectorAtlas` | BakedVector |
| vector winding rule | `path-winding-rules`を`FillRule::NonZero/EvenOdd`としてatlasへ保持し、Slug E flagで直接coverage評価 | Native |
| vector inside/outside stroke geometry | Figma `outlineStroke()`の確定形状をAOT化 | BakedVector |
| text family / numeric weight / italic | `TextVisual` / `TextStyle` + explicit font registry | Native |
| mixed styled text runs | `TextRun[]`（family/size/weight/style/decoration/spacing/line-height/fill） | Native |
| clip content | `clipsChildren`。矩形は高速clip、角丸はGPUの祖先maskで処理。Figma corner smoothing 非0は近似 | Native / Approximated |
| component instance | resolved tree + main-component/source metadata。runtimeは共有definition / instanceを扱うが、exporterはmain-definition参照へdedupしない | Approximated |
| BOOLEAN component property | `property bool` + `visible: $property`としてlive bindingを保持 | Native（対応field） |
| TEXT component property | `property string` + `text: $property`としてlive bindingを保持。mixed/rich textはresolved snapshot + warning | Native / Approximated |
| VARIANT / INSTANCE_SWAP / SLOT | provenance / 値を保持。動的tree selection / swap / slot substitutionは非対応 | Approximated |
| variable / style token | Figma `resolveForConsumer(node)`でmodeとaliasを解決して数値化。参照style IDはmetadataへ保存 | Native snapshot / Approximated dynamic token |
| blur、shadow、blend mode、mask | 警告と対応部分の出力。自動raster fallbackは行わない | Approximated / Unsupported |

色・text・vectorは対応するsemantic dataで保持します。linear gradientはFigmaの`gradientTransform`の第1行（node座標→gradientの線形パラメータ）から`start/end`相当の軸を厳密に導き、回転・傾き・非等方scaleでもgradientの位置を維持します。2-stopではstop位置に合わせて軸を調整し、3～256-stopでは全stop位置と色を`linear-stops()`へ出力します。Frame / Rectangleの複数fillは`fill-layers`へすべて記録します。非線形gradientと特殊なblend modeはまだ近似が残ります。
clipは矩形の高速経路を維持しつつ、角丸frameの`clipsContent=true`ではGPUに祖先の角丸clip maskを渡して子をクリップします。Clip contentsは子要素に作用し、frame自身のcenter/outside strokeを切り取りません。frameのstrokeを子要素より後に描き、center strokeの内側半分がchildに覆われないようにします。Figma独自のcorner smoothingが非0の場合はsuperellipseで近似し、`W_ROUNDED_CLIP_SMOOTHING`を報告します。対応外のeffect、image paint、複雑な合成を完全再現するものではありません。
警告・placeholder・fidelity reportを確認してください。
任意のplugin data、非表示authoring layer、prototype transitionを無条件にruntimeへ持ち込みません。
Figma documentがDisplay P3の場合はplugin側でlinear-light matrix変換してsRGBへ格納し、rendererは
hexをlinearへ一度だけdecodeします。これによりtext/fill/strokeで同じ色空間契約を共有します。

## 現在のexchange契約

format v3 pluginは選択中のnode群をFigmaのpainter順に
`Absolute selection_root`へまとめて
`.slugui`を直接出力します。各nodeのstable nameにはFigma node IDを含め、さらに次のmetadataを
保持します。

数十nodeを一度に選択でき、同時に選ばれた親の子nodeは二重出力しません。rootのサイズは
選択nodeの`absoluteRenderBounds`の和集合なので、外側strokeやgroup内の描画が
`absoluteBoundingBox`だけで切り落とされません。単一選択も同じwrapper契約を使うため、
drop先はcomponent名や選択数に依存しません。compilerは既存format v1/v2も引き続き受理します。

- format version comment、元のFigma document/node ID。Figma由来elementのruntime stable IDは
  `figma/<document>/<node>`から生成し、rename/re-groupでstate identityを変えない
- node名とcomponent/variant由来。`source-provider-data`にはnode type、parent、component key、
  component set、main component、variant/property定義・値をcanonical JSONで保持
- Figma component propertyのうち現在runtime意味へ安全に落とせるBOOLEAN/TEXTは、
  component-localなstable property IDを生成し、`.slugui`のtyped propertyと
  `visible` / `text`の直接bindingとして保持。metadata JSONをrender時に解釈しない
- parent/child順、visible、clip、overlay hint
- layout mode、size mode、min/max constraint、padding、spacing、alignment。cross-axis FILLは
  `align-self: stretch`、auto-layout内の`layoutPositioning=ABSOLUTE`はflow非参加のx/y配置として保持
- fill / `fill-layers`（複数fillを背面→前面で保持）、Rectangle stroke paint/辺別width/align、textのrange別
  family/size/numeric weight/italic/underline/strikethrough/letter spacing/line height/fill/text case
- vectorのnode相対fill geometry、outline stroke geometry、paint、厳密な相対配置
- linked paint styleとVariablesを、Figmaファイルを変更せずasync参照取得・`resolveForConsumer`による適用mode/alias解決で静的RGBA・opacity・stroke幅へ変換。style IDはmetadataへ保持
- import fidelity、warning code、bake理由

SVG path dataはレビュー可能な文字列として`path-data` / `stroke-path-data`へ保持し、compilerが
M/L/H/V/C/S/Q/T/A/Z（相対commandを含む）を厳密にAOT変換します。fill geometryと外側へ広がる
stroke geometryは別ShapeIdでも`path-placement` / `stroke-placement`で元nodeの同一座標系を
保持します。SVG/FigmaのY下向き座標はAOT時にSlugのY上向きへ反転し、表示上の上下を維持します。
Figmaの`absoluteTransform`を最優先し、必要な場合は`relativeTransform`を直近の**位置基準コンテナ（Frame等）**へ合成してworld行列を復元します。`GROUP`と`BOOLEAN_OPERATION`はFigmaのrelativeTransformにおいて透明な祖先となるため、直近のGroupを基準にして再度位置を加算しません。world行列を直接の親Groupのworld行列で逆変換し、各nodeの幅・高さ、親ローカルの移動量と回転・傾斜の2×2行列を別々に出力します。例えばColorPickerの矢印では、Groupのx=428pxに対し元の`relativeTransform` x=473pxを解き、符号を反転した行列とともにGroup内x=45pxとして保存します。描画時は幅5pxの反転により視覚上x=40pxに表示されます。format v1が出したpage座標のvector placementはcompilerが旧形式に限ってlocal boundsへ
再中心化します。v2はFigmaのnode-local geometryとoutline boundsをそのまま保持します。生成componentは
SVGをAOTでC++ path命令へ展開するため、frame描画中にSVG文字列をparseしません。binary imageは
`.slugui`へbase64埋め込みません。node IDとsource metadataは診断と再出力時のidentityへ使います。runtimeからremote URLを取得しません。

## Figma開発プラグインの更新確認

開発プラグインはFigma側で動作するJavaScriptです。SlugiaのEXEを再ビルドしても、**既に開いている旧Figmaプラグインのコードは置き換わりません**。2026-11-22の実検証では、8:18にダウンロードしたファイルと7:17のファイルがSHA-256完全一致であり、旧Exporterの実行が継続していました。

この問題を避けるため、旧プラグインとは**異なるID・名前の新しい開発プラグイン**を用意しています。Windowsで`powershell -NoProfile -ExecutionPolicy Bypass -File tools/update_figma_dev_plugin.ps1`を実行すると、次の場所に最新コードとUIを配置し、ハッシュで一致を確認します。

```text
D:\Developer\slugia\dist\share\slugia\figma-slugui-v5\manifest.json
```

Figma Desktopで旧「SlugUI Exporter」ウィンドウを閉じ、**Plugins → Development → Import plugin from manifest...** から上記を読み込んでください。新たに追加された「**SlugUI Exporter - Fidelity v5**」を実行すると、ウィンドウの上部に`Fidelity exporter 2026-11-22-fidelity-v5`が表示されます。

新しい`.slugui`の先頭には必ず次の2行があります。

```text
// SlugUI exchange format 3, generated by the Figma selection exporter.
// exporter-revision: 2026-11-22-fidelity-v5
```

旧Exporter由来の`.slugui`を新SlugiaのFigma Import画面へドラッグすると、**OLD FIGMA EXPORTER**を表示して取り込みを拒否します。改名や再ダウンロードで内容が同一なファイルを誤認しません。実データを検査するには`python tools/inspect_figma_export.py "C:\path\to\export.slugui"`を使います。Figmaで色相バーが`linear(#ff0000, #ff0000)`としか公開されない場合、色が変化しないことを`W_FLAT_GRADIENT`として明示します。

Figmaプラグインの「Figma PNG」は、現在選択中のFrameを**Figma自身の描画エンジン**で1倍PNG出力します。このReference PNGとSlugiaのGPU描画を比較するため、次を実行します。

```powershell
# 実際の最新.sluguiをexamples/figma_sample.sluguiにimport/ビルド後
D:\Developer\slugia\build\Release\slugia_figma_import_gpu_tests.exe --capture-ppm D:\Developer\slugia\build\slugia_frame.ppm
python tools/compare_figma_reference.py "C:\path\to\Color_Picker.figma-reference.png" "D:\Developer\slugia\build\slugia_frame.ppm"
```

比較器は元画像のピクセル誤差（全体と領域別）、最も合うx/yオフセット、差分ヒートマップを出します。**画像を平行移動して誤差を隠さず、元の座標での誤差も必ず表示します**。従来の「白い四角・赤い棒」を正解とするテストは、旧SlugUIスナップショットの描画スモークとしてのみ扱い、Figma完全再現の証拠とはしません。

既存の生成C++ヘッダーと起動中の旧EXEも新しい`.slugui`に自動追従しません。新ファイルをプレビューへドラッグするか再ビルドしてください。WindowsでEXEがロックされている場合は`tools/build_windows_release.ps1`が内容ハッシュを含む別名EXEとして配置し、旧EXEを強制終了しません。

## Figma library / variables の出力ポリシー

Figmaが`node.fills` / `node.strokes`に公開する実効値を優先します。
styleに紐づくのにpaint配列が未展開の場合は`figma.getStyleByIdAsync`で取得します。
paintとnodeの`boundVariables`は`figma.variables.getVariableByIdAsync`で読み、
`Variable.resolveForConsumer(node)`を使ってそのnodeに適用されたmodeとalias chainを
解決します。参照は解除相当の具体値へ変換しますが、**Figma元データは一切変更しません**。
style IDはsource metadataに保存します。動的Figmaトークンはruntimeに持ち込みません。

角丸clip maskとlinear multi-stop PaintはネイティブのSlugia描画経路へ追加しました。
2-stopおよび3–256-stopのlinear gradientの色とストップ位置を維持します。
Angular/Radial/Diamondの2～256ストップは`*-stops`として全色・位置・gradientTransformを保持し、Vulkanで計算します。色空間やFigma独自の補間との差を含め、ピクセル完全一致には実デザインとの画像比較が必要です。画像paint、
blend/effect、Figma独自corner smoothing等は引き続き段階的な対応が必要です。
単にExporterで代替色へ書き換えることは完全な対応ではありません。

回帰テスト（Vulkan SDK不要）:

```powershell
node tests/figma_plugin_test.js
node tests/figma_token_test.js
python -m unittest discover -s tests -p slugui_compiler_test.py -q
python tools/slugui_compiler.py path/to/ColorPicker.slugui --check
```

## fidelityを隠さない

`ImportFidelity`は次の意味で使用します。

- `Native`: SlugUIの意味と描画で再現
- `Approximated`: 編集可能だが一部の見た目/constraintを近似
- `BakedVector`: Slug vector shapeとして保持するが内部構造は編集不可
- `BakedRaster`: 画像assetとして保持
- `Unsupported`: 出力せずerrorまたは明示placeholder

`BakedRaster` は識別値として定義されていますが、このexporterはbitmap assetの自動生成を行いません。
EVENODD windingは出力・描画へ保持し、fidelity分類にはApproximatedが付く場合があります。

plugin UIはnode別レポートを表示し、AOT compilerはsyntax/type errorをfile/line/column付きで
停止します。strict fidelity modeは提供しません。
「変換成功」と表示しながら黙って見た目やinteractionを落とす実装にはしません。

## 再importと手書きコード

生成ファイルは完全に再生成可能なdirectoryへ分離し、手書きC++を上書きしません。
callbackはstable callback IDまたはgenerated interfaceへ接続し、implementationはユーザー側に
置きます。再出力はexport IDとsource metadataを保持します。永続的な手書きstateとの自動mergeは行いません。

推奨構成は次のとおりです。

```text
ui/
  source/*.slugui              # Figma出力または人手編集する宣言
  generated/*.slugui.hpp       # AOT生成物
  assets/<content-hash>.*      # bakeされたresource
src/
  ui_actions.cpp               # 手書きcallback/state連携
```

## 角丸のストローク幾何と描画順（2026-11-22）

Figmaのinside/center/outsideの縁取りでは、**元の形状の角丸半径を正規化したあと**にストローク幅を差し引く・足す必要があります。以前は最初に幅を差し引き、その後で外形と内形の角丸を独立に正規化していました。例として高さ36px・半径50pxの丸ボタンに左右非対称のinsideストロークを付けると、角の内側輪郭が約2pxずれました。

今は元のFrameから有効半径（この例では18px）を一度求め、その正規化済み輪郭に上下左右各々のストローク幅を反映します。ストロークの輪郭は引き続き同じベクター二次曲線で生成されるため、ラスタ画像への置き換えはありません。

もう一つの不具合は**不透明なストローク**でした。旧処理は外側リングを描いてから、内側に縮めた塗りを描き直していました。これによりグラデーションのorigin/targetも内側の矩形で再解釈され、内側エッジ付近の色がずれ、アンチエイリアスされたリングを塗りが覆っていました。現在は全塗りを元の矩形で計算し、ストロークを後から重ねるFigmaの描画順に統一しています。

GPU回帰テスト `slugia_figma_colorpicker_v5` は以下をチェックします。

- 実際のColorPickerの色相バー内側3pxをx=436..438とx=462..464、中央ストローク2pxのプレビューFrameをx=543..544とx=637..638で検査する
- inside、center、outsideの6px半透明リングが幾何学的に所定の位置へ描かれる
- 不透明なinside幅10pxの輪郭内にある線形グラデーションが、元のFrame座標を保つ
- 高さ36px・半径50px・左ストローク10pxの丸ボタンで、以前約2pxずれた内側角のGPUピクセルが正しい側へ分類される

本件の検査画像は以前のFigmaスクリーンショットも参考にしましたが、**ネイティブFigma PNGとの全ピクセルの一致を確認したわけではありません**。

## Figma Stroke v6：アウトライン精度と共通AA輪郭（2026-11-22）

旧実装には2種類の独立したバグがありました。

**A：Figmaのoutset stroke配置。** ColorPickerのHandle（楕円）では、元図形25×25px、Figmaの表示stroke width 0.938px、外側アウトライン図形26.875×26.875pxです。旧ExporterはミラーしたShapeでstroke-placementを省略し、SlugUI AOTが[0,0,26.875,26.875]として扱ったため輪郭が右下方向へずれていました。修正後はローカル座標[-0.9375,-0.9375,26.875,26.875]に配置し、stroke幅自体を丸め直さず、FigmaのoutlineStroke()で返った実図形サイズを優先します。新Exporterではstroke-placementをpx指定で出力し、従来のv5ファイルはAOTコンパイラで判別可能なケースのみ互換補正します。`0.938px`は正規の小数幅であり、整数pxへのスナップを行いません。

**B：INSIDE strokeのサブピクセル漏れ。** 旧Rendererは塗りAA coverage Fと内側ストロークリングのAA coverage Rを独立してsource-over合成し、白＋白の最終coverageをF+R×(1−F)としていたため、同じ塗り輪郭を超えて余計に濃く表示しました。実機テストで最大64/255の余剰coverageが確認されました。新方式は同一フラグメントでSlugの外側(O)、元の塗り(B)、内側(I)の3輪郭を評価し、内側の塗りとstrokeの重なりも幾何学的に合成してGPUへ一度だけ書き込みます。幅0.938、1、3pxと小数座標3通りで、INSIDEの最終coverageが元の塗りを超えないことを実測確認しています。CENTER・OUTSIDEについても0.938pxの最終輪郭を外側形状と比較しています。左右反転したProjected DrawList内のdescriptor参照も再ベースして、同じAA方式を適用します。各ストロークは1 GPU instanceで描画され、旧2回のアルファ合成を排除します。

現在のGPU単一pass合成は**Solid色stroke**と、同じFigma図形に付く**Solid/Linearの複数fill-layer（最大64枚）**をサポートします。塗り同士をピクセル内で先にsource-over合成し、元の形状のAAを1回だけ適用してからinside/center/outsideのstrokeを同じフラグメントで評価します。Gradient strokeやその他の複雑なfill-layerは従来の経路です。また`clip contents`付きFrameで子レイヤーの前面にストロークを遅延描画する経路では、子の合成とFrame境界を別描画するため、残る微細なAA差があり得ます。Figma純正PNGとの全画面差分検証は未実施です。

最新版の新しいFigma開発プラグインは既存のv5と別IDなので、Figmaのキャッシュを使い回しません。

```text
D:\Developer\slugia\dist\share\slugia\figma-slugui-v6\manifest.json
```

Figma → Plugins → Development → Import plugin from manifest... から追加し、「SlugUI Exporter - Stroke v6」を実行します。ファイル先頭のrevisionは `// exporter-revision: 2026-11-22-stroke-v6`です。元デザインを変更せず再エクスポートできます。

## Figmaのグラデーション・アルファ・ストローク（2026-11-22）

今回のColorPickerで見つかった色差の主因は、VulkanのSRGB swapchain上でRGBを線形化してから半透明を合成していたことです。Figma UIと同じsRGB値上のsource-over合成では、白の上に黒50%を重ねるとRGB約128になります。従来の線形合成ではRGB約188になり、暗いグラデーションが明るい灰色に見えていました。

Figma Importを開けるサンプルGUIでは RendererConfig::preferSrgbAlphaCompositing=true を使用し、SRGB_NONLINEARのUNORM swapchain形式を優先します。デフォルトRendererConfigはこれまでどおり線形RGB経路で、一般のレンダラーを変更しません。GPUがUNORM形式を公開しない環境では従来方式にフォールバックするため、完全な合成一致は保証できません。

実機GPU読み出しでは左中央 (35,217) RGB(127,123,123)、左下 (35,394) RGB(11,11,11)、右中央 (395,217) RGB(127,5,5) を確認しました。専用テスト slugia_figma_colorpicker_v5 はこれを回帰検出します。さらに幅6pxのinside/center/outside枠を異なる位置で検査し、白50%を黒に重ねた値約128、および50%黒を2枚重ねた場合のRGB約64を検査します。FrameのClip contentsと中央ストロークの前後関係は既存のGPUテストでも扱います。

Figma純正PNGとの全ピクセルの一致は未検証です。非Normalブレンドモード、グループの隔離opacity、色空間指定gradientについては依然として追加実装が必要です。

## v5以降のFigmaインポート・EXE更新の整合性（2026-11-22）

従来は `dist/bin/slugia-example.exe` のように**ビルド先からコピーしたEXE**を起動してFigmaファイルをdropすると、別のビルド先の`Release/slugia-example.exe`が再コンパイルされても、**最初のコピーを再び起動**していました。その旧EXEに`--import-success`を付けていたため、「Drop import succeeded」と出てもグラデーションは旧表示のままでした。

現在は`tools/rebuild_example.cmake`が`BUILD_DIR`の実際にビルドされたEXEを検証・再起動します。さらにSlugUI AOTコンパイラは入力の正規化UTF-8 FNV-1a指紋を生成C++内へ埋め込み、Figmaプレビューの起動時にディスク上の`.slugui`と照合します。異なる場合、旧バイナリが**「成功」表示で古い形を出すことを禁止**し、`STALE COMPILED FIGMA DATA`を表示します。

反転・回転のあるFigmaデザインは`ProjectedDrawListCommand`を含みます。現行VulkanのRetained DrawListでは投影コマンドを保持できないため、その場合は**適切なライブベクター描画**へ自動切り替えます。非投影デザインは従来の高速Retained描画を使用します。

色再現の回帰テストは、可変`examples/figma_sample.slugui`から独立した`tests/fixtures/colorpicker_v5.slugui`を使用します。`slugia_figma_colorpicker_v5`は、色選択エリアの右上が**赤**で下側が**暗色**、色相バーの中央が**緑・シアン**であることを実際にGPUで検査します。旧式の白い領域や赤一色のバーは成功しません。`slugia_figma_gui_import_smoke`は本物のGUIページで、投影を含むカラーUIが例外なく描画できることを検査します。

以下のコマンドでFigma GUIを直接検査できます。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/build_windows_release.ps1
& 'D:\Developer\slugia\build\Release\slugia-example.exe' --figma --import-success
```

`dist/bin`からインポートしても、以後の自動再起動先は**検証済みのビルドディレクトリ**です。旧EXEの強制終了や、別名EXEへの黙った差し替えは不要です。

## Exampleでのdrop再読込

source treeからビルドしたExampleのFigma Importページでは、任意の有効なcomponent名を持つ
`.slugui`をウィンドウへdropできます。AOT出力型だけをExample用の固定名へoverrideするため、
単一selectionと複数selectionの両方を扱えます。Exampleだけがfixtureを置換し、現在の
Debug/Release構成を別processでAOT再ビルドしてFigmaページを開き直します。同じsessionですでに取り込んだ
同一bytesのfileは何も変更せず再起動もしません。空のsessionでは同じfileも再度取り込めます。
変更fileは現在のwindowを閉じる前にcompilerでgenerated headerまで一度だけ生成し、構文・型エラーなら
元fixtureへ即時rollbackします。成功時は同時にCMake用codegen stampを更新し、そのheaderをそのまま
buildへ渡すため、helper側で同じPython parse/codegenを繰り返しません。生成bytesが同一ならheaderの
mtimeは維持されるので、見た目/IRが変わらない再exportではC++ compileも発生しません。さらに巨大な
Figma generated headerは`figma_import_bridge.cpp`だけが
includeし、このpreview bridgeだけを低最適化でcompileします。`kitchen_sink.cpp`全体をFigma変更のたびに
再compileしないため、大規模component setでも反復importのcompile latencyを抑えます。library/runtimeへ
`.slugui` parserは追加していません。build失敗時は元fixtureを復元し、詳細を
`build/slugui_drop_rebuild.log`へ保存します。配布アプリで任意UIを動的ロードする契約ではなく、
Figma exporterを反復確認するための開発用導線です。

通常起動では前回の取り込みdataを復元せず、Figma Importページは空になります。`--figma`は
ページを選ぶだけで、dataを読み込みません。生成componentとそのatlas geometryを作るのは、drop成功後に
helperが`--import-success`を渡して開いたsessionだけです。`--import-failed`では読み込みません。
取り込み元fileとAOT成果物は削除せず、通常起動の初期化経路から切り離しています。問題のあるdataで
import sessionが終了しても、EXEを引数なしで起動し直せばそのdataを実行せず開けます。

## Clip contentsのベクター輪郭と合成精度（2026-11-22）

Figmaの角丸Frameに重なる半透明の白い子要素の縁が明るくはみ出す問題を、描画ジオメトリとレイヤー合成の両方で修正した。

**角丸クリップの共有ジオメトリ:** 旧Vulkanフラグメントシェーダーは、Frame本体をSlug二次曲線、`Clip contents`を別のスーパー楕円SDF＋矩形AABBの強制打ち切りで評価していた。小数位置でマスクと実形状の輪郭が異なり、GPU比較では最大255/255のピクセル差があった。現在は親Frameに使用する`roundedRectCurvesCached`と同じ二次曲線をクリップ用のGPU curve/bandテーブルで再利用し、`dynamicSlugCoverageAt`でピクセルを評価する。Clip contents用CPU clipRectは曲線がある場合に1pxの保守的なマージンを加える。整数座標の軸平行・角丸なしclipは従来のハードウェア矩形クリップを維持する。スムージング、.375pxなどの小数位置を変えた比較の最大差は1/255。回転した親Frameから子へ継承されたマスクは親の**world座標**で評価し、明示的にローカルと指定されたProjected DrawListのマスクのみ`localPosition`で評価する。マスクの座標系をGPUデスクリプタに明示し、投影されたカーブとバンドの参照を正しくリベースする。通常の回転Frameと逆回転子要素でもクリップが壊れないことを別のGPU回帰テストで検証する。

**全面半透明オーバーレイの単一輪郭AA:** 赤い角丸Frameに同じマスク領域を完全に覆う50%白の子Rectangleを描く場合、2回の独立したAA合成は外周に最大32/255の色差を生んでいた。通常のDrawList入力で、親のマスク・角丸・順序が一致し、子がマスク全面を覆い、ソリッド／線形ペイントで追加ストロークがなく、元の描画順序を安全に維持できる場合だけ、複数ペイントを`roundedRectLayers`へ統合する。GPUは内部で色を先に重ね、外側の被覆率を一度だけ掛ける。結果の差は3/255以内。部分的に重なる子、異なるパス／マスク、他の描画操作を挟むケースは統合しない。オブジェクト階層、hit-test用図形、Figma元データは変更しない。

**検証:** `tests/figma_import_gpu_test.cpp`は角丸のマスク＝実図形ピクセル比較、SlugUI Runtime経由のclip/CPU scissor、小数位置・スムージング、半透明白の単一AA、部分重なりで統合しないこと、2枚以上の塗り順序、左右反転した投影マスクをテストする。

**残る範囲:** 任意の異なるパスが交差する複雑な半透明子レイヤー同士は、共通の全面マスクでの合成統合対象ではない。一般的なオフスクリーングループ分離を実装したわけではないため、そのような場合までFigmaとピクセル完全一致するとはまだ主張できない。実際のFigma純正PNGとの全画面差分比較も未実施。
