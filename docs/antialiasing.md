# Anti-aliasing / alpha-group fidelity (2026-11-22)

## Canonical build folder (2026-11-22)

All new Slugia builds must use `D:\Developer\slugia\build`, not a sibling of the repository.
The superseded sibling build folders were consolidated. Historical GPU captures,
benchmark JSON, logs and pre-fix reference binaries are retained in
`D:\Developer\slugia\build\research` with source-prefixed filenames.
`research\merge_manifest_20261122.json` records source paths and SHA-256 hashes.
Historical records mentioning retired build-folder names refer to that archived
evidence; do not recreate those directories. GUI distributables remain in `dist\bin`.


## OPEN — P0 rendering-quality defect (not resolved)

**Status as of 2026-11-22: UNRESOLVED.** The user still sees visible
jagged/stepped edges and uneven antialiasing on the **outer and inner rounded
red rectangle** of the ColorPicker, even after the zoomed checker-mask coordinate
fix. The latest user-provided screenshot (2026-11-22) shows this clearly.
Do **not** describe antialiasing as fixed or production-complete.

Earlier passing GPU tests primarily tested straight sides, checker interior,
selected subpixel positions, and limited matched-mask cases. The 56/56 passing
regressions establish that those specific checks pass, **not** that every curved
edge, nested clip, group-alpha edge, or zoom state is visually correct.

### Next implementation work — acceptance gates

1. Reproduce both **outer and inner rounded corners** from the real Figma
   ColorPicker, including red translucent overlays, at zooms below/above 100%,
   with fractional translation and rotated/affine transforms. Capture actual
   pixel output, including near-corner 1–2px strips; do not measure only the
   checker interior.
2. Compare against an independent high-resolution reference *and* an actual
   Figma-exported raster reference. Classify coverage errors separately from
   child/parent clip coverage, source-over blend order, premultiplied alpha,
   gamma/color-space conversion, and geometry/transform mapping.
3. Address the root cause in the **general** vector/mask compositor, preserving
   Fill/Stroke precision and compatibility. In particular, test when several
   semi-transparent child shapes share one rounded mask, and when isolated
   group compositing is required instead of per-child mask multiplication.
4. Add deterministic GPU regression tests for fractional corner curvature,
   edge alpha, zoom stability and nested rounded clips. Include max/mean error,
   edge-location discrepancy, intermediate-alpha values, and annotated
   difference images; do not accept a good average masking bad corner pixels.
5. Record CPU/GPU time, buffer allocations, VRAM, and idle submissions against
   a pre-change Release baseline. No unconditional multisampling, blur, or
   screen-specific patches to conceal jaggies.

Keep this issue **OPEN** until the curved-edge and transparency defects are
reproduced, fixed, and validated independently. The current zoom test and
checkerboard improvements are real, but are **not** a resolution of this P0.

## Target: Figma transparency slider without jagged checker intersections

The original `tests/fixtures/colorpicker_v5.slugui` imports a white clipped 29×388 rounded frame, a much larger even-odd compound checker path (8 SVG path entries / 96 closed contours) with gray paint, and a vertical red→transparent gradient child. Prior to this revision the parent clip, gray atlas path and gradient evaluated separate fractional alpha edges, leading to visible overcoverage near rounded corners; the Slug atlas axis-weighted intersection approximation also generated checker corner outliers.

### Shipping changes

* **Adaptive even-odd vector edge filtering:** `src/shaders/vector.frag` now uses Slug's analytic root coverage at one pixel for fully interior/exterior pixels and evaluates four half-footprint subpixels ONLY on fractional even-odd boundaries. Both native atlas shapes and converted/persistent dynamic storage shapes use the same algorithm. Avoids expensive multisample render targets, blur, geometry displacement and full-screen supersampling.
* **Exact clipped group isolation for matching Frame masks:** `DrawList::shape` recognizes a clipped atlas child whose immediate parent is an equal mask-qualified `RoundedRectCommand`. The following full-bleed solid/linear layers remain in source painter order. `RoundedRectCommand::compositeLayers` records the group. `VulkanRenderer::appendRoundedRectSlug` encodes up to 32 layers with optional atlas geometry (each in the same existing band SSBO), evaluated and source-over composited within ONE GPU fragment, before ONE enclosing Slug rounded contour mask. This avoids fractional overcoverage and does not allocate a second render image. A child with an independent clip, a transformed or sheared shape, an incompatible paint, a stroked parent, or a nonmatching radius is NOT merged.
* **Mask coordinate consistency:** unprojected masks use interpolated `localPosition` to avoid float-roundoff discrepancies between `gl_FragCoord` and filled Slug contours; explicit projected-local masks still use their tagged local coordinate frame. Research/high-performance atlas conversion passes the same mask-vs-fill test as lowSpec.
* **Painter order:** `roundedRectLayers()` preserves all child paint layers when its initial paint was fused into a parent group; a fully off-frame atlas child never forces a missing GPU geometry instance.
* **AA workload cap:** the experimental conditional 4×4 integration had much higher GPU wall time on the existing test; it was removed and the 2×2 boundary-only route retained. Always compare actual GPU timestamps and target hardware before enabling extra samples.

### GPU image validation (RTX 2070 SUPER, ColorPicker native 787×434 test)

A simple 16× area-supersampled reference was generated for only the checker *interior* rectangle x=493..515, y=33..401, using the 8 authored SVG paths, their even-odd winding and original color stack. This does NOT constitute a full Figma-native screenshot comparison.

| Measured absolute channel error /255 | Old | Adaptive 2×2 + masked single-pass group |
|:--|--:|--:|
| Maximum across interior crop | 13.27 | 4.78 |
| Mean | 0.340 | 0.282 |
| Channel samples with error above 5 | 18 | 0 |

Reproducible checker AA validation: python tools/validate_colorpicker_aa.py --actual build/aa_group.ppm (optional Pillow+NumPy). This area-integrates 96 authored checker SVG contours at 16x and compares actual GPU pixels; it is NOT a native Figma rasterizer. Rounded Clip/Fill fractional smooth contour GPU tests now also run under lowSpec and vectorPerformance. The rendered preview smoke and standard GPU quality regressions remain separate checks.

### Scope and limitations

This single-pass grouping is a correct fast path for **adjacent, same-mask compatible layers**, not a universal isolated group renderer. Fully general noncoincident paths, nested groups with arbitrary affine transforms, complex blend modes, text/image filters and independently masked child groups need explicit isolation semantics (and usually a tightly bounded temporary render target) to guarantee color-space and pixel-coverage correctness. The exact Figma-native rendered PNG has NOT been captured and compared for the whole document; do not claim universal 1:1 parity.

Do not choose implementation complexity/extra GPU sampling on visual intuition alone. The renderer must improve pixel-error, GPU time, CPU submit, memory budget and 0-idle redraw simultaneously, and must back out changes that fail a fidelity or performance gate.


## Follow-up: oriented subpixel AA and rectangular Clip intersection (2026-11-22)

- **Screen-axis-correct 2×2 adaptive positions:** both atlas and dynamic even-odd paths now use `dFdx()` and `dFdy()` vectors to place their four quarter-pixel samples. The prior code used the magnitudes of `fwidth()` as *positions*, losing rotation/shear/reflection direction and misplacing samples under projective DrawLists. The analytic half-footprint remains conservative and unchanged, the adaptive trigger is unchanged, and **no additional samples** are introduced. This fixes the subpixel *sample placement*, not full analytic area integration for arbitrary rotated parallelograms.
- **Exact rectangular-mask intersection:** the Vulkan clip-table builder intersects axis-aligned zero-radius rectangles if and only if they share identical `inverseWorld` and `projectiveLocal` coordinates. It emits one Slug mask for their geometric intersection instead of `min(maskA,maskB)`, which is mathematically wrong at perpendicular fractional-pixel crossings. Empty intersections remain on the previous path rather than making invalid zero-sized paths. Nonrectangular/independently transformed masks are not altered.
- **Zero-allocation common path:** a single-mask chain uses its existing vector; multi-mask chains alone allocate a temporary vector for compaction.
- **New GPU regression:** `tests/figma_import_gpu_test.cpp` compares two intersecting fractional rectangular masks against their analytically identical intersection rectangle, including edges and a subpixel corner. It is not tied to the ColorPicker.
- **GPU ColorPicker reference confirmation:** current ColorPicker v5 captured via the RTX 2070 SUPER Release GPU test after the first two changes is exactly identical in RGB across the full 787×434 native capture to the pre-patch `slugia-lowpower-build` GPU test (0 differing channel samples). Against the independent 16× checker reference: max 4.7793/255, mean 0.28207/255, and 0 channel samples above 5. The comparison is reproducible with `D:\Developer\slugia\build\research\aa-review__compare_aa.py` and the source-prefixed `aa-review__before.ppm`/`aa-review__after.ppm` there.

General isolated compositing of arbitrarily clipped nested translucent child groups is still *not* implemented. This optimization has a well-defined rectangular-coincident coordinate-space scope; arbitrary curve intersections will require separate rigorous area-integration tests. Performance impact must be measured on identical workloads before claiming net throughput or power improvement.

- **Disjoint-rectangle no-op:** if two compatible rectangular masks do not intersect, the compiled mask chain emits a reserved empty marker (no path geometry); the shader rejects it exactly. This prevents false faint intersections when two separate AA fringes overlap across a subpixel gap. The ColorPicker GPU regression includes a 0.375px-gap negative-control case.


## Performance guardrail — unresolved regression (2026-11-22)

**Do not claim a throughput or power improvement from these AA changes yet.** On RTX 2070 SUPER, the retained static Ghostscript Tiger SVG benchmark (`slugia-vector.exe --benchmark --samples 32 --doublings 0 --seconds 45 --frame-budget-ms 1000`, same GPU/driver/configuration/asset and Release compiler settings) gave baseline GPU p95 measurements of **2.3934, 2.1900, 2.1690 ms**, versus revised measurements of **3.2642, 3.4669, 2.8445 ms** in alternating runs. A subsequent 64-sample run: baseline **2.1241 ms**, revised **3.7566 ms**. These are GPU timestamps on a shared desktop, not power measurements. Both binaries passed the throughput test's intentionally loose 1000 ms budget, which does not validate performance parity. The old `slugia-lowpower-build` SPIR-V `vector.frag.spv` was 354,228 bytes versus 404,900 bytes in the new build, but shader size alone does not prove causality. Prioritize reducing shader work / compiler expansion for general even-odd paths before considering this optimization a complete performance win.

The exact ColorPicker capture is unchanged at the native pixel level and the intersecting/disjoint rectangle regression checks pass. A correct final release also needs **A/B GPU profiling under the same clock and workload**, GPU/CPU memory and actual idle power measurements. Keep the old faster EXE as a known throughput reference until the regression is resolved; do not overwrite it.


## Final local verification and staging (2026-11-22)

- Release build: `D:\Developer\slugia\build\Release`, Visual Studio 2022, Windows x64 / Vulkan SDK; MSBuild exit 0.
- Entire final CTest suite: **56/56 PASSED**, zero failed, `D:\Developer\slugia\build\research\aa-review__ctest-final.log` (GPU, SVG, ColorPicker, 3D, compiler, idle smoke).
- ColorPicker final GPU capture: `D:\Developer\slugia\build\research\aa-review__after.ppm`; checker reference max **4.7793/255**, mean **0.28207/255**, all 787×434 native RGB capture pixels identical to old pre-patch build. The archived script and generated `aa-review__after.png`/`aa-review__change_heatmap.png` live in `build/research`.
- Staged an explicitly **non-overwriting**, SHA-addressed EXE: `D:\Developer\slugia\dist\bin\slugia-aa-reviewed-20261122-e96ae27d1287.exe`. The staged file passed both `--smoke` and `--idle-smoke`. Idle smoke confirmed **5 waits, 0 additional submissions, 0 draw-list rebuilds**.
- **Performance warning remains:** the revised static SVG GPU benchmark is slower on RTX 2070 SUPER (old ~2.12–2.39 ms p95 versus revised ~2.58–3.76 ms across repeats). Do not replace the previous efficient executable or advertise a GPU speedup from this change. Power, general-group isolation and full-document Figma PNG parity are still unverified.
- The worktree's pre-existing uncommitted changes were preserved. No GitHub push was performed.


## Zoom/pan invariance of masked paint stacks (2026-11-22, follow-up)

### Root cause fixed in the generic fragment shader

The `vector.frag` fused masked-stack shader previously computed a child's `layerUv` from `(localPosition - child.position) / child.size`. In a retained DrawList view, `localPosition` has already been transformed into **framebuffer coordinates** while the encoded child position and em bounds remain in **authored DrawList coordinates**. Thus moving or zooming the view sampled entirely incorrect regions of an even-odd child path (and often made the checker disappear). More AA samples did not solve this.

**Implementation:** The nonprojective masked child now maps screen position back into authored space with `(localPosition - translationOverride.xy) / viewportScale.zw` before calculating the child's UV and Slug em coordinate; projective batches continue using their already-preprojective `localPosition`. No global AA sample-count change, extra render target, per-frame CPU geometry rebuild, or new GPU resource was introduced. The normal 100%/zero-translation path produces the same pixels.

### Real GPU ColorPicker multi-zoom verification

The GPU regression executable supports `--capture-zoom SCALE OUTPUT.ppm`. It isolates the **actual imported three-layer checker+red-opacity rounded Frame**, rather than synthetic image texture, and renders at 50%, 75%, 90%, 100%, 112.5%, 150%, and 200% zoom with a nonzero translation. `tools/validate_colorpicker_zoom.py CAPTURES_DIR 16` independently integrates the 96 authored even-odd checker contours at 16x and applies the authored red alpha ramp inside the exact straight-sided frame, excluding corner regions from its strict flat-rectangle reference. This is **not** a full Figma-native PNG reference.

| Zoom | Max RGB error before /255 | Max RGB error after /255 |
|---|---:|---:|
| 0.50 | 39.72 | 3.82 |
| 0.75 | 39.54 | 3.60 |
| 0.90 | 39.46 | 5.48 |
| 1.00 (translated) | 39.55 | 4.34 |
| 1.125 | 39.66 | 3.57 |
| 1.50 | 29.60 | 2.36 |
| 2.00 | 22.60 | 1.69 |

Measured grayscale checker-vs-white tile contrast is **17/255 at all seven zoom factors**, while previously the child checker could disappear. The ordinary rectangular-clip-only analytic area test also passes with <1/255 error at all seven zoom factors. The ColorPicker native untransformed image remains covered by the older independent 16x checker reference regression.

The automated GPU regression includes zoom/pan checker center-point comparisons and a 90-degree projective rotation comparison. Remaining: exact Figma native screenshot parity for all layer types, arbitrary noncoincident translucent isolated groups, extreme zoom, and a performance gate (the prior generic SVG GPU slowdown remains separately tracked).


### Reproduce seven zoom captures (PowerShell, using an out-of-tree Release build)

```powershell
$build = 'D:\Developer\slugia\build'
$exe = Join-Path $build 'Release\slugia_figma_import_gpu_tests.exe'
foreach ($z in @('0.5','0.75','0.9','1.0','1.125','1.5','2.0')) {
  & $exe --capture-zoom $z (Join-Path $build "zoom_$z.ppm")
  if ($LASTEXITCODE -ne 0) { throw "GPU capture failed: $z" }
}
python 'D:\Developer\slugia\tools\validate_colorpicker_zoom.py' $build 16
```

The baseline captures are saved as `zoom_before_*.ppm` in the same build folder and the post-fix images are `zoom_*.ppm`. The two independent metrics JSON files are `zoom_metrics_before.json` and `zoom_metrics.json`. This test intentionally compares the *rendered* matched-mask group under view zoom, not a zoomed raster screenshot.


### Final validation — zoom-coordinate fix

- Release build completed successfully using VS2022 x64/Vulkan.
- Final full CTest: **56/56 PASS**, 0 failed; see `D:\Developer\slugia\build\research\aa-review__ctest-zoom-fix.log`. Tests include all preexisting GPU/quality checks and the newly added seven-zoom/checker + 90-degree projected mask assertions.
- The full native untransformed ColorPicker GPU capture (`zoomfix_native.ppm`) was compared against the saved pre-fix `before.ppm` at 787×434 and has **0 changed RGB channel samples**. The new correction only changes transformed masked-paint evaluation.
- The 16× independent zoom reference validator succeeded for all seven cases with maximum error at most **5.476/255**, vs pre-fix maxima up to 39.724/255. The seven zoomed GPU captures, before captures and JSON reports are kept in the separate build directory.
- The GUI smoke test and zero-idle-submit smoke test succeeded with **5 empty waits and no extra GPU frames or draw-list rebuilds**.
- New SHA-addressed, non-overwriting GUI EXE: `D:\Developer\slugia\dist\bin\slugia-aa-zoomfix-20261122-bb8f9c77e3bf.exe`. The copied EXE hash matches the Release-built source binary.
- New retained SVG 256-sample A/B performance test on RTX 2070 SUPER, with alternating runs, recorded baseline GPU p95 **2.446 / 2.367 ms**, revised **2.480 / 2.489 ms** (see `zoomfix_perf_256_*.json`). The differences are within several percent but not a proof of throughput parity or power efficiency; older short-run comparisons showed larger variability. The masked-group fix introduces no extra samples, no new target, and no additional idle frames. A stricter calibrated GPU/power analysis is still outstanding.
- No source changes were committed or pushed to GitHub, and existing EXEs were not replaced.
