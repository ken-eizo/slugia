# Slugia UI performance and idle power (2026-11-22)

## Principle

The first performance target is **avoiding a frame that contains no new information**. The former desktop example called `prepareFrame()`, rebuilt the entire DrawList and submitted Vulkan work as fast as the GPU could present, even when the Figma canvas was unchanged. It also selected IMMEDIATE presentation by default. With no event pacing this burns CPU time, command-buffer bandwidth, GPU compute, VRAM bus and power to repeat identical pixels.

## Implemented

- `Window::waitEventsTimeout(seconds)` uses native GLFW event waiting, preserves InputState edge delivery, and returns whether a meaningful window/input callback actually fired. It rejects negative/non-finite timeouts. Cursor, scroll, keyboard, mouse, text, drop, resize, refresh, focus and iconify callbacks invalidate the view. `PlatformSurface::requestRedraw()` posts a thread-safe explicit wakeup through `Window::postRedrawEvent()` so asynchronous redraw signals are not mistaken for timeouts; normal polling consumes old requests before event dispatch to avoid losing concurrent notifications. The Figma Import, Text Zoom, SlugUI IR and Vector3D views rebuild and submit only after an event; five consecutive empty waits produce no additional submissions (regression test `slugia_example_idle_smoke`). The animated Components demo remains refresh driven.
- The desktop example uses refresh-synchronized FIFO presentation by default rather than unrestricted IMMEDIATE. The existing `--mailbox` opt-in keeps MAILBOX behavior; `--uncapped` explicitly allows IMMEDIATE. Only the time-driven Components animation updates continuously at display refresh; Vector3D reacts to user controls and therefore sleeps when unchanged. Do not use event-only idle logic for continuously animated pages, video, asynchronous redraw or timers unless they explicitly post redraw requests.
- The `RendererConfig::interactiveUi()` profile keeps vector compaction and continuous corner precision, but reduces cache budgets and initial buffer sizes. The pre-existing `RendererConfig::vectorPerformance()` remains available through `--throughput` when throughput with many complex/dynamic shapes is more important than footprint.
- Resize refresh callbacks now acquire/present after the new size event instead of preparing one frame early and submitting two during Windows' modal sizing loop. Normal and idle smoke tests cover this.

## Memory budgets, not measured full VRAM

| Resource | vectorPerformance | interactiveUi |
|---|---:|---:|
| Curve + band GPU buffer configured budget (combined) | 16 MiB | 4 MiB |
| CPU dynamic path cache budget | 4 MiB | 2 MiB |
| CPU dynamic shape cache budget | 2 MiB | 1 MiB |
| Resolved glyph runs limit | 512 | 256 |
| Initial vector instance capacity | 4,096 | 1,024 |

These budgets are **not** the Windows process working set, actual GPU VRAM usage or a guaranteed peak. Device, swapchain, atlas, glyph data and temporary buffers also consume memory. Shrinking a cache can increase CPU compilation if a workload exceeds it; use `--throughput` for that case.

## Measurement and regression

Use `slugia-example.exe --idle-smoke` for five event-free waits and assert zero new GPU frame submissions and draw-list rebuilds, `slugia-example.exe --smoke` for native resize/input/presentation, and `ctest --output-on-failure` for the Vulkan/Core/SlugUI/SVG/3D suite. Profile `RendererStats` (CPU build/upload/submit, GPU timestamp with interval enabled, draw calls, quads and uploaded bytes) and external process working set. `nvidia-smi` reports the GPU as a whole, not Slugia-exclusive utilization on WDDM. Compare identical contents, window dimensions, refresh rate, driver, and GPU power policy before claiming speedups. Idle should be zero submitted frames, not a fabricated FPS.

## AA and masked-group work remaining

The Figma transparency slider is a nested checkerboard **even-odd compound vector path** over white plus a semi-transparent red gradient in a clipped rounded Frame. Matching the clip boundary to the same Slug path greatly improved parent/child contour agreement, but does not by itself guarantee optimal filtering of the checker texture or exact compositing of arbitrary overlapping semi-transparent shapes. The existing fusion is valid for adjacent full-bleed Solid/Linear paints with coincident masks only. A general solution for noncoincident opaque/transparent child shapes is an isolated, premultiplied-alpha clip group (ideally tightly bounded and retained), with the rounded mask applied **once after** child source-over composition. That requires separate render-target lifecycle, cache invalidation, blend-space tests, and GPU/memory benchmarks; it is not yet implemented. Do not add brute-force MSAA without profiling, and do not claim Figma PNG pixel parity without an authored Figma reference export.
