# Display backends

Select a backend with `-DGRAPHICS_BACKEND=...`:

| Backend | Output |
| --- | --- |
| `FRAMEBUFFER` | Emu68's existing framebuffer, discovered through `devicetree.resource`. |
| `CHIPSET` | Real Amiga custom chips; excludes framebuffer dependencies. |
| `COMBINED` | Framebuffer when `devicetree.resource` exists; chipset otherwise. Default. |

A malformed framebuffer description causes initialization to fail; it never
falls back to chipset access on a PC. Framebuffer output keeps the existing
mode and yields to normal Picasso96 display ownership.

On the framebuffer backend, `VBeamPos` reports a logical scan position.
`QBlit` and `QBSBlit` silently decline hardware-dependent requests, without
invoking callbacks or reporting completion. The chipset backend uses real
beam, Copper, bitplane, sprite and blitter hardware.

Use `-DGRAPHICS_BACKEND=CHIPSET -DGRAPHICS_CPU=68000` for an original Amiga CPU.
The linked build generates `graphics.library.footprint.txt` for ROM sizing.
Both backends remain under development.

## Attribution

Hardware identification and fetch/scroll timing adapt AROS
[`setchiprev.c`](https://github.com/aros-development-team/AROS/blob/3793aefc6f2d8498b9629eec852e1613cf69f21f/arch/m68k-amiga/graphics/setchiprev.c) and
[`amigavideo_chipset.c`](https://github.com/aros-development-team/AROS/blob/3793aefc6f2d8498b9629eec852e1613cf69f21f/arch/m68k-amiga/hidd/amigavideo/amigavideo_chipset.c),
revision `3793aefc6f2d8498b9629eec852e1613cf69f21f`, under APL-1.1.
Copyright (C) 1995–2020, The AROS Development Team. See `LICENSE.APL` and source notices.
