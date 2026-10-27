# ビルド・組み込み・実行

## 要件

- CMake 3.24以降、C++20対応のMSVC / Clang / GCC、Python 3.10以降。
- Vulkan SDKのheaders / loader / `glslc`。実行するGPUはVulkan 1.1以上。
- GLFWを使う場合はwindow system。LinuxではGLFWのX11 / Wayland開発パッケージも必要。
- macOSはVulkan SDKのMoltenVK、AppleのC/C++ toolchain。
- Node.jsはFigma exporterのテストに使用。通常描画の依存ではない。

依存ライブラリのrevisionはGit submoduleで固定しています。

Linuxのwindow開発依存は[GLFWのビルド要件](https://www.glfw.org/docs/latest/compile.html)に従います。
Debian / Ubuntuの例は `sudo apt install libwayland-dev libxkbcommon-dev xorg-dev` です。

```sh
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

SDKを自動検出できない場合は `VULKAN_SDK`、または `-DVulkan_ROOT=/path/to/sdk` を指定します。
WindowsではVisual StudioのC++ desktop workloadを使用します。macOSではSDKの設定scriptを読み込んでから実行します。
Vulkan portability enumeration / subsetは対応deviceで自動的に有効にします。
FreeType / HarfBuzz / GLFWの外部サンプル・試験は本体のビルドでは無効です。

## CMake設定

| 変数 | 内容 |
| --- | --- |
| `SLUGIA_BUILD_EXAMPLE` | GUI、SVG、3Dサンプル。top-levelではON |
| `SLUGIA_BUILD_TESTS` | CPU・toolchain回帰。top-levelではON |
| `SLUGIA_BUILD_GPU_TESTS` | GPU・画像比較。既定OFF。GLFWと表示可能なVulkan環境が必要 |
| `SLUGIA_BUILD_BENCHMARKS` | CPU / SVG benchmark。既定OFF |
| `SLUGIA_ENABLE_3D` | 3D実装。既定ON、OFFでビルドから除外 |
| `SLUGIA_ENABLE_GLFW` | `Window` adapter。既定ON、OFFでも外部surfaceで描画可能 |
| `SLUGIA_ENABLE_VALIDATION` | debug build用validation設定。runtimeの `RendererConfig::validation` と併用 |
| `SLUGIA_STATIC_MSVC_RUNTIME` | MSVC runtimeをstatic link |
| `SLUGIA_ENABLE_SANITIZERS` | GCC / Clangのaddress / undefined sanitizer。既定OFF |
| `SLUGIA_INSTALL` | raw static archive / header / toolのinstall。top-levelではON |

サンプルはGLFWを必要とします。GLFWを外す場合は `SLUGIA_BUILD_EXAMPLE=OFF` も指定します。
3Dなし・GLFWなしの構成も同じ公開2D APIを使えます。

## サンプル

| 実行ファイル | 内容・引数 |
| --- | --- |
| `slugia-example` | GUI、SlugUI、Figma、文字zoom、3D。`--figma` / `--vector3d` で該当ページを起動 |
| `slugia-vector` | 採用した性能設定でTiger / SVGを表示。`--svg file.svg`、`--low-spec`、`--baseline` |
| `slugia-vector-baseline` | 互換設定の比較用viewer |
| `slugia-3d` | 3D viewer。`--asset model.obj` / `--asset model.slug3d`、`--low-spec`、`--baseline` |

Windowsは `.exe` が付きます。macOSではGUI / 3Dサンプルを `.app`、vector viewerを通常の実行ファイルとして生成します。
`--smoke` は有限frameで終了するvalidation試験です。SDKのvalidation layerが必要です。
vector / 3Dの `--smoke --capture image.ppm` は読み戻したRGBを保存します。vectorではbenchmarkとの併用も可能です。
通常起動はfontとshaderを実行ファイルへ埋め込み、vector viewerの既定Tigerも埋め込んでいます。
外部SVG / modelと、Exampleのsource再生成を伴う `.slugui` dropは明示指定したfile / 開発workspaceを使います。

## 既存CMake projectへ組み込む

```cmake
set(SLUGIA_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SLUGIA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(path/to/slugia)
target_link_libraries(my_app PRIVATE slugia::slugia)
```

```cpp
#include <slugia/slugia.hpp>

int main() {
  slugia::VectorAtlas atlas;
  const auto box = atlas.addPath(slugia::Path{}.rect(0, 0, 1, 1));
  atlas.build();
  slugia::Window window;
  slugia::VulkanRenderer renderer(window, atlas,
      slugia::RendererConfig::vectorPerformance());
  slugia::DrawList draw;
  while (!window.shouldClose()) {
    renderer.prepareFrame();
    window.pollEvents();
    const auto size = window.framebufferSize();
    draw.clear();
    draw.setClip({0, 0, size.x, size.y});
    draw.fill(box, {20, 20, 160, 80},
        slugia::Paint::solid(slugia::Color::fromRgb8(0x62dcff)));
    renderer.draw(draw);
  }
  renderer.waitIdle();
}
```

順序は **atlas登録→build→renderer作成→prepare→event→DrawList→draw** です。
atlasとwindow / surfaceはrendererより長く保持します。通常は同じUI/render threadから呼びます。
例外はアプリケーション境界で扱います。Vulkan device lossの自動復旧は提供しません。
`setRefreshCallback` でresize中の再描画を登録でき、終了前にcallbackを解除してください。
`WindowConfig::manageGlfwLifetime=false` はhostがGLFWの初期化・終了を所有する場合に使います。

`cmake --install build --prefix path` はarchive、header、AOT tool等を配置します。
これは依存込みの `find_package(slugia)` packageではありません。source組み込みは上記targetを使ってください。

## 試験

```sh
cmake -S . -B build -DSLUGIA_BUILD_GPU_TESTS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

validation layerを検出できない環境はSDKの `Bin` / layer manifest directoryを `VK_LAYER_PATH` へ指定します。
GPU試験は順次実行します。unsupported / skipと成功は区別します。
