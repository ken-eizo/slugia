# Slug3D v1 — semantic vector 3D interchange

Slug3D preserves authoring geometry rather than baking a screen-space quality setting into
the file. slugia lowers the visible result to its vector coverage pipeline at the current
view's tolerance. Mesh polygon triangulation and adaptive curve subdivision are runtime details.

## v1 JSON container

Top level: `format="slug3d"`, `version=1`, coordinate system, and `objects[]`.
Each object stores a world transform and one semantic geometry payload.

Geometry kinds:
- `curve`: native Bezier/NURBS/poly splines, cyclic state, control points and handles,
  plus Blender bevel/extrude metadata. Bezier curves are never sampled by the exporter.
- `mesh`: indexed vertices + polygon faces + authored sharp edges. Polygons are deliberately
  not triangulated in the file. Import/lowering may reconstruct planar regions, creases and
  silhouettes before a triangle fallback is considered.
- `analytic_hint` may occur as metadata. This reader does not replace the source mesh/curves
  with an analytic primitive based on that hint.

## Invariants

1. Source geometry is authoritative; no fixed quality/tessellation setting is part of Slug3D.
2. Curves retain their source controls. Surface fallbacks retain the evaluated mesh.
3. Mesh topology stays indexed and shared; no triangle soup is required by the format.
4. Raster AA is not serialized. Coverage remains the responsibility of slugia.
5. Importers must preserve a lossless mesh fallback whenever semantic recognition fails.
6. Optimization must not alter geometry merely to hide rendering artifacts.

## Implemented reader and viewer

`parseSlug3D` reads a UTF-8 JSON string, `loadScene3D` reads `.slug3d` or `.obj`, and
`drawAsset3D` lowers the asset into a shared scene. `slugia-3d --asset file.slug3d`
loads, frames and displays it. `examples/vector3d_demo.slug3d` is an executable format example.

The reader supports `blender-z-up-rh` and `slug-y-down-z-forward-rh`. A Blender position
`(x,y,z)` becomes `(x,-z,y)`; world matrices are converted by the same basis change.
`matrix_world` contains four row-major rows. Material colors are sRGB RGBA.

Mesh payloads contain `vertices`, original polygon `faces`, optional `sharp_edges`,
`materials` (RGBA arrays) and `face_materials` (one index per original face). The reader keeps
the original indexed polygons alongside the triangles used for visibility.

Bezier splines contain `points` with `p`, `in`, and `out` vectors. Poly/NURBS points contain
`[x,y,z,weight]`. NURBS additionally accepts `degree`, `endpoint`, `bezier_knots`, an optional
explicit `knots` vector, and `cyclic`. Positive rational weights are retained. Prepared spans
do not replace the source controls.

Curves with nonzero `bevel_depth` or `extrude` include an evaluated `mesh_fallback` from the
exporter. Both source curves and fallback geometry are retained; the surface fallback is used
for display. Analytic hints are metadata and never replace unvalidated source geometry.

Input bytes, vertices/control points, face size, triangles, JSON nesting and work are bounded.
Duplicate JSON keys, invalid indices, invalid weights and unsupported versions are rejected.
Non-manifold surface topology is diagnosed rather than silently changing the source.

## Blender exporter

Blender 4.0以降で `tools/blender_slug3d_export.py` をaddonとしてinstall / enableするか、
Text Editorで読み込んでRun Scriptを実行します。File > Export > slugia (.slug3d) から出力します。
Selected Objectsは選択中のMESH / CURVEだけを出力します。OFFはscene全体を対象にします。
Bezier / NURBS / polylineのsource controls、world matrix、color / face materialとsharp edgeを保持します。
bevel / extrudeを持つcurveは評価済みmesh fallbackも出力します。解析hintによる形状置換は行いません。
fileの具体的な全fieldは `examples/vector3d_demo.slug3d` とexporterを参照してください。
