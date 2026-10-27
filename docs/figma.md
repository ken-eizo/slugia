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
| Rectangle stroke paint / weight / align | `StrokeVisual` + 解析stroke ring | Native |
| Rectangle side-specific stroke weights | `[top, right, bottom, left]` + 非対称解析ring | Native |
| corner smoothing | `CornerSmoothing` 0–100% | Native |
| solid / linear / diamond / radial paint | 共通`Paint` | Native |
| opacity | `Paint::opacity`またはelement opacity拡張 | Native / Approximated |
| vector fill/center stroke geometry | node相対`path-data` -> `Path` -> `VectorAtlas` | BakedVector |
| vector winding rule | `path-winding-rules`を`FillRule::NonZero/EvenOdd`としてatlasへ保持し、Slug E flagで直接coverage評価 | Native |
| vector inside/outside stroke geometry | Figma `outlineStroke()`の確定形状をAOT化 | BakedVector |
| text family / numeric weight / italic | `TextVisual` / `TextStyle` + explicit font registry | Native |
| mixed styled text runs | `TextRun[]`（family/size/weight/style/decoration/spacing/line-height/fill） | Native |
| clip content | `clipsChildren` | Native |
| component instance | resolved tree + main-component/source metadata。runtimeは共有definition / instanceを扱うが、exporterはmain-definition参照へdedupしない | Approximated |
| BOOLEAN component property | `property bool` + `visible: $property`としてlive bindingを保持 | Native（対応field） |
| TEXT component property | `property string` + `text: $property`としてlive bindingを保持。mixed/rich textはresolved snapshot + warning | Native / Approximated |
| VARIANT / INSTANCE_SWAP / SLOT | provenance / 値を保持。動的tree selection / swap / slot substitutionは非対応 | Approximated |
| variable / style token | 解決済み値とmetadata。動的token tableは非対応 | Approximated |
| blur、shadow、blend mode、mask | 警告と対応部分の出力。自動raster fallbackは行わない | Approximated / Unsupported |

色・text・vectorは対応するsemantic dataで保持します。gradientはfirst / last stopの2色へ変換します。
clipは矩形です。対応外のeffect、image paint、複雑な合成を完全再現するものではありません。
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
- fill、Rectangle stroke paint/辺別width/align、textのrange別
  family/size/numeric weight/italic/underline/strikethrough/letter spacing/line height/fill/text case
- vectorのnode相対fill geometry、outline stroke geometry、paint、厳密な相対配置
- 現在はresolved design token/variable値と明示warning
- import fidelity、warning code、bake理由

SVG path dataはレビュー可能な文字列として`path-data` / `stroke-path-data`へ保持し、compilerが
M/L/H/V/C/S/Q/T/A/Z（相対commandを含む）を厳密にAOT変換します。fill geometryと外側へ広がる
stroke geometryは別ShapeIdでも`path-placement` / `stroke-placement`で元nodeの同一座標系を
保持します。SVG/FigmaのY下向き座標はAOT時にSlugのY上向きへ反転し、表示上の上下を維持します。
Group/Booleanの特殊なrelative transformには`absoluteBoundingBox`を使うため、親原点の二重加算を
行いません。format v1が出したpage座標のvector placementはcompilerが旧形式に限ってlocal boundsへ
再中心化します。v2はFigmaのnode-local geometryとoutline boundsをそのまま保持します。生成componentは
SVGをAOTでC++ path命令へ展開するため、frame描画中にSVG文字列をparseしません。binary imageは
`.slugui`へbase64埋め込みません。node IDとsource metadataは診断と再出力時のidentityへ使います。runtimeからremote URLを取得しません。

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
