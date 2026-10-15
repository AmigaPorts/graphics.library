# Open Source reimplementation of `graphics.library` for AmigaOS 3.x
[![CMake](https://github.com/amigaports/graphics.library/actions/workflows/cmake.yml/badge.svg)](https://github.com/amigaports/graphics.library/actions/workflows/cmake.yml)

An open-source replacement for Kickstart's `graphics.library`, based on
AmigaOS 3.2.3 behavior and preserving the classic API and Picasso96 compatibility.
Development is ongoing; full behavioral compatibility is not yet achieved.

## Why?

To liberate AmigaOS and open source it under a fair license, piece by piece.

## Building

Build from this directory with an m68k AmigaOS toolchain and CMake:

```sh
cmake -S . -B build -DM68K_CPU=custom -DM68K_FPU=custom -DM68K_CRT=custom \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=./install
cmake --build build --parallel
cmake --install build
```

`GRAPHICS_BACKEND` selects `COMBINED` (default), `FRAMEBUFFER` or `CHIPSET`.
`GRAPHICS_CPU` defaults to `68040`; use `CHIPSET` with `68000` for an original
Amiga CPU. Framebuffer builds require at least a 68020. See [backends](CHIPSET.md).

The project builds independently. It produces the library, public developer
headers and a generated header for extension-ROM embedding.

## Development

The priorities are Emu68 framebuffer support, real Amiga chipset support, then
optimization. Shared graphics behavior serves both backends. Scaling and
masked-pattern edge cases still have known compatibility failures.

Testing uses original AmigaOS comparisons and [p96cts](https://github.com/codewiz/p96cts).
[VolAmos loader support](https://github.com/sidick/volamos/issues/133) is tracked upstream.

## Performance

Relative time per operation: **lower is better**, original = **1.00**.
Emulator measurements are provisional, not real-hardware results.

Current pixel benchmark: 4 planes, FS-UAE 3.2.35, 68040/AGA, fixed approximate
CPU timing, **JIT disabled**. Original references are reused for unchanged
CPU/chipset, ROM, emulator and benchmark settings. Pixel outputs match.

| Operation | Original | Replacement | Previous |
|---|---:|---:|---:|
| ReadPixel, unlayered | 1.00 | **1.52** | 1.72 |
| WritePixel, unlayered | 1.00 | **1.73** | 1.90 |
| ReadPixel, layered | 1.00 | **1.42** | 1.50 |
| WritePixel, layered | 1.00 | **1.53** | 1.62 |

These focused timings are separate from the earlier SysSpeed baseline below.

SysSpeed 2.6, FS-UAE A1200/68040, PAL 640×512, 16 colours. This baseline
predates the blitter-ownership and pixel-access optimizations in `0365b02`
and `4643f17`; repeat runs varied substantially.

| Test | Original | Replacement |
|---|---:|---:|
| Horizontal scrolling | 1.00 | 1.50 |
| Vertical scrolling | 1.00 | Unverified |
| Text | 1.00 | 1.41 |
| Rectangle fill | 1.00 | 1.37 |
| Draw ellipse | 1.00 | 1.39 |
| Draw circle | 1.00 | 1.39 |
| WritePixel | 1.00 | 1.92 |
| Horizontal lines | 1.00 | 1.30 |
| Vertical lines | 1.00 | 1.23 |
| Diagonal lines | 1.00 | 1.37 |
| Filled ellipse | 1.00 | 0.94 |
| Filled circle | 1.00 | 0.82 |
| Area fill | 1.00 | 0.75 |

Vertical scrolling needs output verification before its timing can be compared.

## License

Library code is MPL-2.0, with attributed APL-1.1 components and separately
licensed fonts. See [public headers](HEADERS.md), [font sources](FONTS.md)
and the accompanying `LICENSE*` files.
