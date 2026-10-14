# Built-in fonts

## Topaz 8

`src/topaz.c` derives from Alex Limi's
[Topaz Double Sans](https://github.com/amigavision/TopazDouble/tree/c7eb6f8fe40b8f0533ee3f5e76b7f28ea43e8ee8),
revision `c7eb6f8fe40b8f0533ee3f5e76b7f28ea43e8ee8`, file `Fonts/Topaz Double Sans/16`.
Copyright (c) 2024 Alex Limi; MIT license, preserved in `LICENSE.Topaz`.

The conversion removes duplicate raster rows and uses 8×8 Latin-1 font metrics,
baseline 6. No glyph data was extracted from Kickstart.

## Topaz 9 (default)

`src/topaz9.c` contains Michael Malyshev's Topaz/9 50.1 A1251 from
[AROS's Amiga-1251 fonts](https://github.com/aros-development-team/AROS/tree/084dfe66344b67591d34b7adb318d0fc203ddce7/workbench/fonts/Amiga-1251/topaz_Amiga-1251),
revision `084dfe66344b67591d34b7adb318d0fc203ddce7`, file `9`.

Glyphs and metrics are unchanged: 10×9 cells, baseline 7, Amiga-1251 encoding.
This is a Cyrillic font, not a Latin-1 copy of Kickstart Topaz 9.

Its original distribution terms are preserved in `LICENSE.Topaz9`; retain that
notice with source and binary distributions. These data are not relicensed
under MPL, MIT or APL. Both font strikes are included in the standalone project.
