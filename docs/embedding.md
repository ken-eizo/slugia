# 外部surface・入力・Vulkan連携

## 所有権

`VulkanRenderer` はVulkan instance / device / queue、surface、swapchainと描画資源を所有します。
`PlatformSurface` と `VectorAtlas` はcallerが所有し、rendererより長く保持します。
`RenderDevice` はatlasと設定の公開factory、`RenderSurface` はrendererを所有するmove可能なhandleです。
この版の複数surfaceはそれぞれのrenderer / Vulkan deviceを作成します。保持IDやexternal bufferをsurface間で共有しません。
APIは単一UI / render thread契約です。callback、resource更新、draw、resize、破棄を直列化してください。

## 外部window

`SLUGIA_ENABLE_GLFW=OFF` でも `PlatformSurface` を実装すれば利用できます。

| method | caller側の契約 |
| --- | --- |
| `requiredInstanceExtensions` | instance作成前に必要extension名を返す。文字列を構築中有効に保つ |
| `createVulkanSurface` | 渡されたinstance上でsurfaceを作る。成功したsurfaceはrendererが破棄 |
| `framebufferSize` / `contentScale` | 最新のphysical pixel数とscaleを返す |
| `visible` / `requestRedraw` | host表示状態と再描画要求 |
| `waitForVisibleFramebuffer` | 最小化時の扱い。埋め込みでは既定の非blocking実装を使用可能 |

Windowsには借用HWND用のadapterがあります。HWND自体は破棄しません。

```cpp
#include <slugia/win32_surface.hpp>
slugia::Win32PlatformSurface surface(hwnd, hinstance);
slugia::VulkanRenderer renderer(surface, atlas,
    slugia::RendererConfig::vectorPerformance());
```

HWNDはrenderer破棄後まで維持し、hostのmessage loopからresize / redrawを処理します。
macOSの任意NSView / CAMetalLayerや特定DCCのadapterは同梱しません。
その場合はhost側でsurface extension、native view/layer、DPI、lifetimeを管理してこの契約へ接続します。
standalone macOS windowはGLFW / MoltenVKの経路です。

## 入力

`InputWriter` でhostのeventを `InputState` に渡します。

```cpp
slugia::InputState input;
slugia::InputWriter writer(input);
writer.beginFrame();
writer.cursor({xInFramebuffer, yInFramebuffer});
writer.mouseButton(slugia::MouseButton::Left, slugia::InputAction::Press);
// key、text、scroll、focus等のeventもここで投入
writer.finishFrame(timeSeconds);
```

座標はpanel-local framebuffer pixelです。down stateとpress / release edgeを区別します。
`focusLost` は押下状態とcompositionを解除します。Unicode commit、IME composition / selection / commit / cancel、
scroll gesture、cursor delta、raw motion、key repeatを保持します。
OSのcandidate windowとtext serviceの接続はhost側です。
`Window` はevent投入を行い、`resampleCursor` で宣言直前の絶対位置を再sampleできます。
`UiContext::setClipboardCallbacks` へhostのread / write関数を渡すと編集widgetのclipboard操作に使います。

## VulkanInterop

通常描画には不要です。`<slugia/vulkan_interop.hpp>` をincludeし、`renderer.vulkanInterop()` から取得します。

```cpp
auto& interop = renderer.vulkanInterop();
const auto context = interop.deviceContext();
const auto image = interop.registerRgba32fBuffer(buffer, offset, byteRange);
draw.externalImage(image, bounds, width, height);
// 使用終了時：interop.unregisterExternalImage(image)
```

`buffer` は同じ `context.device` で作ったVkBufferで、登録中は有効に保持します。
pixelはfloat32の **A,R,G,B** の順で4 words、pixel間に隙間を入れません。
`createOwnedRgba32fImage` / `updateOwnedRgba32fImage` はrenderer所有のCPU更新bufferを提供します。
unregisterは安全な完了待ちを伴います。

`setFrameRecorder` は同じcommand bufferへUI render pass前のcommandを記録するhookです。
callbackでsubmit / wait / throwせず、command bufferを保存しません。
`completedSubmissionSerial()` は完了済みsubmissionの上限を非blockingで返します。
ID、device context、callbackは作成元rendererのlifetime内だけ有効です。
異なるdeviceのbuffer、一般的なVkImage import、D3D / Metal外部memory共有のAPIではありません。

hostのABI境界へC++ container / exception / allocator ownershipを直接渡す設計は避け、
libraryと同じcompiler / runtimeでstatic linkしてください。versioned C ABIは提供しません。
