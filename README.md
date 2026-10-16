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

## Compatibility

✅ Matches tested original behavior; ❌ known difference or unsupported;
— not established. These are tested paths, not a claim of complete API parity.

| Operation | Framebuffer | Chipset |
|---|:---:|:---:|
| ReadPixel / WritePixel and layer storage | ✅ | ✅ |
| Bitmap transfers, minterms and clipping | ✅ | ✅ |
| Text layout, font metrics and software styles | ✅ | ✅ |
| Lines, ellipses and area fills | ✅ | ✅ |
| Regions and layer backing storage | ✅ | ✅ |
| Palette and pen management | ✅ | ✅ |
| Screen switching and buffer notifications | ✅ | ✅ |
| Picasso96 display takeover | ✅ | ✅ |
| `p96cts` output parity with original | ❌ | ✅ |
| BitMapScale negative-phase cases | ❌ | ❌ |
| QBlit / QBSBlit hardware callbacks | ❌ | ✅ |
| Physical beam-position reporting | ❌ | ✅ |
| Real-hardware compatibility validation | — | — |

Scaling has unresolved differences between emulator references. The PC/P96
configuration retains a masked-pattern colour discrepancy; chipset comparisons
match the original, including its existing `p96cts` failures. Framebuffer queued
blits are intentionally unavailable, and its beam position is logical.

## Performance

Relative time per operation: **lower is better**, original = **1.00**.
Emulator measurements are provisional, not real-hardware results.

Current pixel benchmark: 4 planes, FS-UAE 3.2.35, 68040/AGA, fixed approximate
CPU timing, **JIT disabled**. Original references are reused for unchanged
CPU/chipset, ROM, emulator and benchmark settings. Pixel outputs match.

| Operation | Original | Replacement | Previous |
|---|---:|---:|---:|
| ReadPixel, unlayered | 1.00 | 1.46 | 1.45 |
| WritePixel, unlayered | 1.00 | 1.62 | 1.62 |
| ReadPixel, layered | 1.00 | **1.33** | 1.39 |
| WritePixel, layered | 1.00 | **1.42** | 1.47 |

SysSpeed 2.6 graphics tests: the same fixed approximate 68040/AGA timing,
JIT disabled, PAL 640×512, 16 colours. The saved original reference is reused.
Previous values precede the constant-template fill optimization. These replace the
older variable-speed baseline; values from the two configurations cannot be compared.

| Test | Original | Replacement | Previous |
|---|---:|---:|---:|
| Horizontal scrolling | 1.00 | 1.80 | 1.80 |
| Vertical scrolling | 1.00 | Unverified | Unverified |
| Text | 1.00 | **2.69** | 3.84 |
| Rectangle fill | 1.00 | 2.37 | 2.37 |
| Draw ellipse | 1.00 | 1.53 | 1.53 |
| Draw circle | 1.00 | 1.53 | 1.53 |
| WritePixel | 1.00 | 2.11 | 2.11 |
| Horizontal lines | 1.00 | 0.23 | 0.23 |
| Vertical lines | 1.00 | 0.91 | 0.91 |
| Diagonal lines | 1.00 | 3.60 | 3.60 |
| Filled ellipse | 1.00 | 2.50 | 2.50 |
| Filled circle | 1.00 | 2.55 | 2.55 |
| Area fill | 1.00 | 4.08 | 4.08 |

Vertical scrolling needs output verification before its timing can be compared.

Focused horizontal copies: off-screen 320×128, 4 planes, same 68040/AGA settings.
Median EClock time for 20 calls across three samples; **separate from SysSpeed**.
Previous values precede the overlap read-ahead optimization. Pixel outputs match.

| Operation | Original | Replacement | Previous |
|---|---:|---:|---:|
| BltBitMap, left | 1.00 | **3.47** | 9.15 |
| BltBitMap, right | 1.00 | 1.78 | 1.78 |
| ScrollRaster, left | 1.00 | **4.20** | 9.65 |
| ScrollRaster, right | 1.00 | 2.36 | 2.35 |

## License

Library code is MPL-2.0, with attributed APL-1.1 components and separately
licensed fonts. See [public headers](HEADERS.md), [font sources](FONTS.md)
and the accompanying `LICENSE*` files.
