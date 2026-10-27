# Third-party notices

slugia includes or adapts the following projects. Their complete license files remain in each
`third_party/` and `examples/assets/` directories. Initialize Git submodules to obtain their full texts.

## Slug

Reference shader implementation by Eric Lengyel. Dual licensed under MIT or Apache-2.0.

> Slug shader code Copyright 2017 by Eric Lengyel.

The algorithm's patent has been dedicated to the public domain. Slug's README requires credit for
distributed software; this notice and the shader source comments provide that credit.

## slughorn

Copyright (c) 2026 AlphaPixel LLC. MIT License. The vendored core and FreeType bridge are actual
dependencies; provenance is recorded in `third_party/slughorn/VENDORED.md`.

## GLFW

Copyright (c) 2002-2006 Marcus Geelnard. Copyright (c) 2006-2019 Camilla Löwy.
Licensed under the zlib/libpng license; see `third_party/glfw/LICENSE.md`.

## FreeType

Copyright (c) 1996-2026 The FreeType Project. This distribution selects the FreeType License (FTL),
available at `third_party/freetype/docs/FTL.TXT`; see `third_party/freetype/LICENSE.TXT` for the full
licensing overview and notices for contributed modules.

Portions of this software are copyright © 2026 The FreeType Project (www.freetype.org).
All rights reserved.

## HarfBuzz

HarfBuzz provides font shaping under its Old MIT license. Copyright notices and permission terms
are retained in `third_party/harfbuzz/COPYING`.

## NanoSVG

Copyright (c) 2013-14 Mikko Mononen. zlib-style license. The full notice and upstream parser
acknowledgements are at the start of `third_party/nanosvg/nanosvg.h`. This project uses its parser.

## Geist

Copyright 2024 The Geist Project Authors. The embedded font uses SIL Open Font License 1.1;
see `examples/assets/fonts/OFL-Geist.txt`.

## Benchmark artwork and research references

The Ghostscript Tiger input is retained byte-for-byte from the fixed Vello asset revision in
`docs/provenance.md`. It is demo/benchmark content. Its source URL and SHA-256 are recorded there
separately from the renderer's code license.

Vello, Skia, Blend2D, Rive, ThorVG and papers are research/comparison references; their renderer
implementations are not linked into this build.
