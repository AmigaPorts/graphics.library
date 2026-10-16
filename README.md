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
All tables use the same measured build; Previous is the last published result.
Emulator measurements are provisional, not real-hardware results.

Focused pixel benchmark: 4 planes, FS-UAE 3.2.35, 68040/AGA, fixed approximate
CPU timing, **JIT disabled**. Original references are reused for unchanged
CPU/chipset, ROM, emulator and benchmark settings. Pixel outputs match.

| Operation | Original | Replacement | Previous |
|---|---:|---:|---:|
| ReadPixel, unlayered | 1.00 | 1.38 | 1.38 |
| WritePixel, unlayered | 1.00 | 1.59 | 1.59 |
| ReadPixel, layered | 1.00 | 1.30 | 1.30 |
| WritePixel, layered | 1.00 | **1.40** | 1.41 |

Results updated with this source change.

SysSpeed 2.6 graphics tests: the same fixed approximate 68040/AGA timing,
JIT disabled, PAL 640×512, 16 colours. The saved original reference is reused.
These replace the older variable-speed baseline; values from the two
configurations cannot be compared.

| Test | Original | Replacement | Previous |
|---|---:|---:|---:|
| Horizontal scrolling | 1.00 | 1.29 | 1.29 |
| Vertical scrolling | 1.00 | 0.47 | 0.47 |
| Text | 1.00 | 1.70 | 1.70 |
| Rectangle fill | 1.00 | 0.96 | 0.96 |
| Draw ellipse | 1.00 | 1.36 | 1.36 |
| Draw circle | 1.00 | 1.35 | 1.35 |
| WritePixel | 1.00 | 1.61 | 1.61 |
| Horizontal lines | 1.00 | 0.16 | 0.16 |
| Vertical lines | 1.00 | 0.90 | 0.90 |
| Diagonal lines | 1.00 | 2.05 | 2.05 |
| Filled ellipse | 1.00 | 1.46 | 1.46 |
| Filled circle | 1.00 | **1.39** | 1.41 |
| Area fill | 1.00 | **1.85** | 1.88 |

Results updated with this source change.

An earlier whole-library `-O3` trial reduced WritePixel time by 29% and horizontal-line
time by 27%, but increased rectangle-fill time by 39% and text time by 3%.
The 68040 chipset ROM grew from 152 to 240 KiB. The default remains `-O2`.

Focused horizontal copies: off-screen 320×128, 4 planes, same 68040/AGA settings.
Median EClock time for 20 calls across three samples; **separate from SysSpeed**.
Pixel outputs match.

| Operation | Original | Replacement | Previous |
|---|---:|---:|---:|
| BltBitMap, left | 1.00 | 2.79 | 2.79 |
| BltBitMap, right | 1.00 | 1.37 | 1.37 |
| ScrollRaster, left | 1.00 | 2.74 | 2.74 |
| ScrollRaster, right | 1.00 | 1.38 | 1.38 |

Results updated with this source change.

Separate-source `BltBitMap`: same dimensions and timing settings, with distinct
source/destination bitmaps. Output hashes match the original.

| Source alignment | Original | Replacement | Previous |
|---|---:|---:|---:|
| 1 bit | 1.00 | 1.99 | 1.99 |
| 15 bits | 1.00 | 2.03 | 2.03 |

Results updated with this source change.

Logical `BltBitMap` operations: same separate-source setup, 1-bit source
alignment. Output hashes match the original.

| Operation | Original | Replacement | Previous |
|---|---:|---:|---:|
| XOR | 1.00 | 2.24 | 2.24 |
| AND | 1.00 | 2.26 | 2.26 |
| OR | 1.00 | 2.29 | 2.29 |

Results updated with this source change.

Aligned logical `BltBitMap`: same setup, 320-pixel width, both memory directions.
Output hashes match the original.

| Operation | Direction | Original | Replacement | Previous |
|---|---|---:|---:|---:|
| XOR | forward | 1.00 | 1.50 | 1.50 |
| AND | forward | 1.00 | 1.53 | 1.53 |
| OR | forward | 1.00 | 1.55 | 1.55 |
| XOR | backward | 1.00 | 1.51 | 1.51 |
| AND | backward | 1.00 | 1.54 | 1.54 |
| OR | backward | 1.00 | 1.56 | 1.56 |

Results updated with this source change.

Masked-clear `BltBitMap` (`~source & destination`): same settings, 320-pixel
aligned or 319-pixel shifted width. Output hashes match the original.

| Alignment | Direction | Original | Replacement | Previous |
|---|---|---:|---:|---:|
| aligned | forward | 1.00 | 1.66 | 1.66 |
| aligned | backward | 1.00 | 1.67 | 1.67 |
| shifted | forward | 1.00 | 2.44 | 2.44 |
| shifted | backward | 1.00 | 2.40 | 2.40 |

Results updated with this source change.

## License

Library code is MPL-2.0, with attributed APL-1.1 components and separately
licensed fonts. See [public headers](HEADERS.md), [font sources](FONTS.md)
and the accompanying `LICENSE*` files.
