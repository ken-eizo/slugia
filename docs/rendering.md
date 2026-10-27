# SVG・保持描画・ズーム・設定・計測

## SVG

```cpp
auto svg = slugia::SvgDocument::load("drawing.svg");
slugia::VectorAtlas atlas;
svg.registerShapes(atlas);
atlas.build();
auto list = svg.drawList(atlas);
```

`SvgDocument(xml)` はUTF-8文字列から構築します。`size()` は文書size、`shapeCount()` はshape数です。
`registerShapes` はatlas.buildより前、`drawList` はその後に呼びます。
Path / rect / circle / ellipse / line / polyline / polygon、group transform、viewBox、単色fill / stroke、
nonzero / evenodd、cap / join / dashとopacityを扱います。塗りは開いたsubpathも暗黙に閉じ、strokeの閉路は保持します。
gradient、filter、mask、一般clip、text要素、CSS stylesheet、animation、scriptはSVG importerの対応外です。
不対応の内容を黙って成功にせず例外で拒否します。描画APIのgradient等とは対応範囲が異なります。
`SvgImportLimits` は既定64 MiB / 200,000 shapes / 4,000,000 control points。DTD / entityも拒否します。

## 動的描画と保持描画

`DrawList` は宣言順を保持し、通常commandの後にoverlayを描きます。`append` はclip / opacityを適用して追加します。
`reserve` は記録容量だけを確保します。`clear` は容量を再利用します。
`dynamicVectorPath` は二次曲線列を所有し、`cacheable=true` で再利用を許可します。
`line` / `arc` / `cubicBezier` はframe中の形状変更を扱います。

```cpp
const auto id = renderer.createRetainedDrawList(list);
slugia::DrawList frame;
frame.setClip({0, 0, 800, 600});
frame.retainedDrawList(id, {20, 20}, 2.0f);
renderer.draw(frame);
const auto transferred = renderer.updateRetainedDrawList(id, changedList);
renderer.destroyRetainedDrawList(id);
```

保持した静的instanceは位置・scale・clip・opacity変更で再構築しません。
`updateRetainedDrawList` はCPUで再構築後、必要な変更範囲を転送し、転送byte数を返します。
同一内容は0 bytesです。nested retained commandとexternal imageを含むlistは保持できません。
IDは作成元renderer専用で、破棄後のIDを再利用しません。
文字は `createRetainedText` / `updateRetainedText` / `destroyRetainedText` と `DrawList::retainedText` を使います。
通常の `text` は文字列を所有、`textStatic` / `textRunsStatic` はdraw完了までの借用です。

## ズームと射影

```cpp
slugia::View2D view{100.0, 100.0, 2.0, {400, 300}};
frame.retainedDrawListView(id, view);
// 文字は frame.retainedTextView(textId, view)
```

`centerX/Y` は文書座標の倍精度中心、`zoom` は正のuniform scale、`origin` はframebuffer上の原点です。
通常倍率は保持GPU dataを使い、大倍率は可視領域を局所座標へ移して曲線を切り出します。
入力自体の精度で失われた細部は復元しません。任意形状で無限倍率を保証するAPIではありません。
`DrawList::projected` は通常2D listにhomographyを適用します。projected listの再入れ子は非対応です。
3D平面は `projectPlane3D` / `drawPlane3D` / `unprojectPlane3D` を使います。

## 設定

`RendererConfig::vectorPerformance()` は1024帯、storage atlas、24-byte保持曲線、可逆band ID圧縮を選びます。
`vectorPerformance(true)` は同じ方式を低メモリ予算で使い、帯を512または原形式へ戻します。
`RendererConfig{}` / `lowSpec()` は互換設定です。性能は入力とGPUで異なります。

| field | 意味 |
| --- | --- |
| `clearColor` / `transparentSurface` | 背景。透明surfaceはnative compositor側の対応も必要。非対応なら作成失敗 |
| `vsync` / `allowTearing` | FIFO / MAILBOX / IMMEDIATEの選択方針。利用可能なpresent modeから選ぶ |
| `enableContinuousCorners` | superellipse形状の角。OFFは円弧へ簡略化 |
| `initialVertexCapacity` / `retainedArenaInitialBytes` | 初期instance / 保持arena容量。必要に応じて拡大 |
| `glyphRunCacheCapacity` | 解決済み文字runの件数予算 |
| `dynamicPathCacheBytes` / `dynamicGeometryCacheBytes` | CPU保持payload予算。0でcache無効 |
| `persistentGeometryBufferBytes` | 曲線用とband用それぞれのGPU buffer。0で昇格無効 |
| `enableAtlasStoragePaths` | atlasを既存固定GPU bufferへ配置 |
| `enableAtlasBandRefinement` / `atlasBandRefinementCells` | 保守的帯細分化。32～2048の2冪、予算内でfallback |
| `enableCompactPersistentCurves` | 使用するfloat32座標を32→24 bytesで保持。transientは元形式 |
| `enablePackedAtlasBands` | eligibleな静的curve ID列を可逆圧縮。範囲外は元形式 |
| `enableIndexedVectorQuads` | 4頂点を再利用する明示的な比較設定。採用presetではOFF |
| `enablePathTileCulling` | 試験的path tiling。画質の包括的な同値性を確認していないため採用presetではOFF |
| `validation` | Khronos validation。layerを別途用意する |
| `gpuTimingInterval` | N submissionごとのGPU timestamp。0で無効 |
| `enableCpuPhaseTiming` | APIを囲むhost wall時間。既定OFF |
| `enableReadback` | `drawAndReadback` を許可。通常描画では無効 |

通常の固定GPU bufferはcurve / band各8 MiB、lowは各64 KiBです。cache予算はcontainer・allocator・font・
swapchain・現在frameの全メモリを含みません。予算超過のcache entryは保持せず、元の内容を描画します。
保持転送のstagingは最大4 MiBを再利用します。入力削減による省メモリではありません。

## 統計と読み戻し

`stats()` はquads / retained / primitive数、draw数、転送byte数、CPU build / upload / submit、GPU時間、
cache使用量とhit / miss、固定bufferの論理量と実確保量、validation状態とerror数、atlas配置・帯配分を返します。
`deviceName` / `presentModeName` は実際の選択を返します。
`FramePixels` はwidth / heightとRGBA8を所有します。読み戻しは同期・メモリ費用を伴います。

GPU timestampは非同期で、`gpuSampleSubmissionSerial` が値の所有submission、`lastSubmittedSerial` が現在のsubmissionです。
同じ行のCPU / GPU値を無条件にpairedにしません。CPU phase診断では末尾wait後にqueryを回収してserialで照合できます。
API区間はOS deschedulingを含み、active CPU cyclesではありません。`waitIdle()` はVulkanの失敗を伝播します。

## benchmark

```sh
cmake -S . -B build -DSLUGIA_BUILD_BENCHMARKS=ON
cmake --build build --config Release
python tools/run_vector_campaign.py --binary build/Release/slugia-vector.exe --samples 200 --doublings 13
python tools/check_benchmark.py build/campaign/latest.json
```

単一構成ではbinary pathから `Release/` を外します。runnerはTiger通常 / low / overdrawを分け、
`build/paris-30k.svg` がある場合はParisも測ります。任意SVGはviewerの `--benchmark --svg file.svg` でも指定できます。
`--samples`、`--doublings`、`--frame-budget-ms`、`--memory-mib`、`--seconds`、`--output`、
`--profile-phases`、`--profile-ranges`、`--overdraw`、`--low-spec`、`--baseline` がviewerの診断引数です。
帯 / storage / compact / packed / indexed / tilingも明示flagで比較できます。sourceのoptions関数に対応します。
runnerは既定p95 16.667 ms、RSS512 MiB、120秒＋準備45秒。最初の失敗まで1→2→4…と進み、
失敗・予算停止・未実行を合格にしません。40 sampleと200 sampleは別profileです。
`run_vector_telemetry.py` は同runnerと500 msのdevice全体のnvidia-smi記録を組み合わせます。
`analyze_cpu_phases.py` は診断の同じCPU行を解析し、`summarize_vector_experiment.py` はhashと集約を保存します。

`run_vector_reliability.py --binary build/Release/slugia_fuzz_tests.exe --doublings 10` は
決定的fuzzの入力規模を倍増し、時間・RSS上限または最初の失敗で止めます。
`compare_spirv_structure.py legacy.spvasm trial.spvasm --output comparison.json` は
SDKのdisassemblyをID正規化して比較する補助toolです。driverの機械語や画像の同値性は証明しません。
`embed_binary.py input output.hpp --symbol name [--namespace app::assets]` は
font / SVGをC++ byte配列へ変換し、CMakeが使用します。`svg_path.py`はAOT compilerのSVG path parserです。
