# 0.1.0の検証と対応範囲

2026-10-27に、公開用sourceからReleaseを新規buildして検証しました。
Windows 10 x64、Core i9-10900、GeForce RTX 2070 SUPER、MSVC 19.44、Vulkan SDK 1.4.357です。
Linux / macOSを対象とする移植コードは含みますが、この版の実機検証結果はWindowsのみです。

## 回帰・画質

GPU tests / benchmarksを有効にしたCTestは **51 / 51成功**、skipなし、584.58秒でした。
CPU geometry、SVG、3D、stress / fuzz、golden、AOT compiler、Figma API mock、計測形式、
Vulkan readback、保持形式の各設定、GUI / 3D viewerのsmokeを含みます。
Figma desktop自体の自動操作や全font / 全SVGの適合証明ではありません。

文字と曲線は、shaderのband選別・root式を使わない倍精度ray intersectionを参照とし、
輪郭付近1.5 pxを除いた内外判定で穴と漏れを調べます。ASCIIの3 weight、3 size、
曲線回帰形状、文字Oの局所View2Dで2^-20〜2^40の倍率を検査します。
輪郭近傍のAA精度や任意のglyphで同じ倍率を保証する試験ではありません。
Tigerのpath / view別比較、色・paint・clip・stroke等のGPU回帰も別に含みます。

公開版の採用設定と`--baseline`を、Tiger通常・low・Parisの1024×1024 RGB全画素で比較し、
3条件とも不一致channel数0、最大差0でした。これらはvalidation有効、error数0です。
path tilingとindexed quadは採用設定で無効です。
有限入力での同値性確認を、全入力の保証に拡張しません。

## 静的保持描画の性能

同一公開binaryの採用設定と`--baseline`を各1回測りました。解像度1024×1024、
IMMEDIATE present、frames in flight 1、各段階warmup 5 / 計測200 frame、nearest-rank p95 / p99です。
GPU timestampは1 ns / 64 bitで、wallとは非同期・非pairedです。
wall区間はprepare / poll / record / submit / presentを含み、import / 初期化 / readbackは含みません。
制御点更新は0回/frame、保持済み描画の転送は0 bytes/frameです。

判定はwall / GPU p95とも16.667 ms以下、process RSS 512 MiB以下、
各case120秒＋準備45秒以内です。1→2→4…と進み、最初の失敗で止めます。
tilesは各copyの投影面積を縮める配置、overdrawは同じ領域へ重ねる配置で、同じ負荷ではありません。
lowは低メモリ設定であり、低性能GPUを使った結果ではありません。

| case | 最終通過copy数 | wall / GPU p95 (ms) | 最初の失敗copy数 | wall / GPU p95 (ms) | case全体のpeak RSS (MiB) |
| --- | ---: | ---: | ---: | ---: | ---: |
| Tiger / tiles | 1,024 | 9.576 / 9.236 | 2,048 | 17.325 / 16.979 | 272.8 |
| Tiger / low | 16 | 14.611 / 14.242 | 32 | 17.971 / 17.522 | 102.0 |
| Tiger / overdraw | 8 | 13.311 / 12.947 | 16 | 26.663 / 26.076 | 117.4 |
| Paris / tiles | 4 | 14.151 / 13.757 | 8 | 23.964 / 23.330 | 505.1 |

上記の失敗はすべて時間基準の未達です。最後の通過段階のwall / GPU p99は、順に
9.827 / 9.294、15.119 / 14.473、13.773 / 13.421、15.005 / 14.516 msでした。
RSSは停止段階を含むcase全体のpeakで、特定frameのGPU常駐量ではありません。

同じcopy数での比較値:

| 入力 | baseline wall / GPU p95 (ms) | 採用設定 wall / GPU p95 (ms) |
| --- | ---: | ---: |
| Tiger 32 copies | 19.009 / 18.655 | 2.979 / 2.734 |
| Tiger low 32 copies | 18.906 / 18.491 | 17.971 / 17.522 |
| Tiger overdraw 4 copies | 28.182 / 27.587 | 6.575 / 6.388 |
| Paris 1 copy | 48.801 / 48.375 | 5.941 / 5.726 |

baselineの最終通過はTiger16、low16、overdraw2、Parisは1個目で未達でした。
これは同機器の単回観測です。別rendererとの同一条件比較や世界最高値の実証ではなく、
反復間変動、cold start、制御点変形、疎更新 / churnの性能をこの表から推定できません。

## 再実行

```sh
cmake -S . -B build -DSLUGIA_BUILD_GPU_TESTS=ON -DSLUGIA_BUILD_BENCHMARKS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
python tools/run_vector_campaign.py --binary build/Release/slugia-vector.exe --samples 200 --doublings 13 --results build/performance
python tools/run_vector_campaign.py --binary build/Release/slugia-vector.exe --samples 200 --doublings 13 --baseline --results build/baseline
python tools/check_benchmark.py build/performance/latest.json build/baseline/latest.json
```

単一構成buildでは`Release/`を外します。runnerはraw値、p95 / p99、実際の設定、binaryと埋め込みTigerのhash、
外部SVGのpath、device情報、RSS、予算停止をJSONへ保存します。外部SVGのhashは別途照合してください。
validatorは計測の整合性を確認し、画質やSOTAを認定しません。
Tigerは同梱assetを使用します。Parisを測る場合は
[固定入力](https://github.com/google/forma/blob/681e8bfd348caa61aab47437e7d857764c2ce522/assets/svgs/paris-30k.svg)
を`build/paris-30k.svg`へ保存してください。SHA-256は
`26ee4c4deaab627448d453ce4dcc9bf15dc674d8330e7fc274ddb10cf8387948`です。
未配置のParisは`corpus_not_downloaded`となり、成功には数えません。

対応する描画意味論・入力上限・所有権は各機能のドキュメントに記載しています。
device lossの自動復旧、任意倍率、SVG全仕様、全Unicode layout、無制限資源での動作は保証しません。
この検証では他OS実機、sanitizer、障害注入、長時間soakは未実行です。
