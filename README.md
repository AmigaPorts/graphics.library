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
| ReadPixel, unlayered | 1.00 | 1.46 | 1.45 |
| WritePixel, unlayered | 1.00 | 1.62 | 1.62 |
| ReadPixel, layered | 1.00 | **1.33** | 1.39 |
| WritePixel, layered | 1.00 | **1.42** | 1.47 |

SysSpeed 2.6 graphics tests: the same fixed approximate 68040/AGA timing,
JIT disabled, PAL 640×512, 16 colours. The saved original reference is reused.
Previous values precede the incremental line-row optimization. These replace the
older variable-speed baseline; values from the two configurations cannot be compared.

| Test | Original | Replacement | Previous |
|---|---:|---:|---:|
| Horizontal scrolling | 1.00 | 9.00 | 9.00 |
| Vertical scrolling | 1.00 | Unverified | Unverified |
| Text | 1.00 | 9.73 | 9.73 |
| Rectangle fill | 1.00 | 4.25 | 4.25 |
| Draw ellipse | 1.00 | 1.53 | 1.53 |
| Draw circle | 1.00 | 1.53 | 1.53 |
| WritePixel | 1.00 | 2.11 | 2.11 |
| Horizontal lines | 1.00 | **8.79** | 10.75 |
| Vertical lines | 1.00 | **8.78** | 10.75 |
| Diagonal lines | 1.00 | **8.52** | 10.51 |
| Filled ellipse | 1.00 | 3.75 | 3.75 |
| Filled circle | 1.00 | 3.89 | 3.89 |
| Area fill | 1.00 | 7.26 | 7.26 |

Vertical scrolling needs output verification before its timing can be compared.

## License

Library code is MPL-2.0, with attributed APL-1.1 components and separately
licensed fonts. See [public headers](HEADERS.md), [font sources](FONTS.md)
and the accompanying `LICENSE*` files.
