# Built-in font sources

`src/topaz.c` contains the size-8 Latin-1 `topaz.font` strike, derived from
[Topaz Double Sans](https://github.com/amigavision/TopazDouble/tree/c7eb6f8fe40b8f0533ee3f5e76b7f28ea43e8ee8),
Copyright (c) 2024 Alex Limi, under the MIT license in `LICENSE.Topaz`.
The upstream project describes the font as a hand recreation. No glyph bytes
were extracted from Kickstart or from GPL font collections for this source.

The input is `Fonts/Topaz Double Sans/16` at revision
`c7eb6f8fe40b8f0533ee3f5e76b7f28ea43e8ee8`, SHA-256
`86bcfb19a08cf17c422cec01bcc6dc5cce1a9f1d6168dd72b8ab4b4c2b52f67e`.
Its 16-row raster consists of eight identical row pairs. The conversion retains
rows 0, 2, 4, 6, 8, 10, 12 and 14, the original 192-byte row stride, and all
225 character-location entries. It changes the header to the size-8 ROM-font
metrics: width 8, height 8, baseline 6, bold smear 1, characters 32 through 255,
plus the fallback glyph. The immutable data is borrowed by writable font
headers managed by `pcInitROMFonts`.

Read-only comparisons against both supplied 47.115 Kickstarts verify every
glyph pixel and width, including the fallback. The ROMs are test inputs only;
they are not a build dependency or a source of redistributed glyph data.

The size-9 strike in `src/topaz9.c` is Michael Malyshev's **Topaz/9 50.1
(02.08.04) A1251**, from the user-selected AROS source:

`workbench/fonts/Amiga-1251/topaz_Amiga-1251/9` at revision
`084dfe66344b67591d34b7adb318d0fc203ddce7`, SHA-256
`86038fe12b0e1487cea1d8b1aa29b4eb4fdd72507699972c44e31ff2c62f3ecc`.

Its accompanying `Amiga-1251_fonts.readme` is preserved verbatim in
`LICENSE.Topaz9`. It permits use on AmigaOS-derived systems and distribution
with AmigaOS, MorphOS, AROS, Amitlon and UAE software/packages. These font data
remain under that notice; they are not relicensed under MPL, MIT or AROS's
blanket APL license. Retain the notice with binary and source distributions.

The raster (9 rows of 322 bytes) and all 257 character locations are unchanged.
The in-memory header retains height 9, width 10, baseline 7, bold smear 1,
extended style and character coverage 0–255 plus fallback. Its charset tag
remains MIBenum 2104 (Amiga-1251). This is the supplied Cyrillic encoding,
not a Latin-1 remapping or an exact copy of Kickstart's size-9 artwork. The user
explicitly selected this AROS font after rejecting the generated alternative.

The disk HUNK wrapper is replaced by immutable C data and the normal writable
startup font header. The font is registered as `topaz.font`; its storage flag
changes from DISKFONT to ROMFONT. The disk-file TAGGED marker is represented
by the tag list passed to ExtendFont, rather than retained as a drawing style.
No raster resampling or changes to glyphs, spacing, kerning or baseline occur.

Startup registers size 9 as the default, followed by size 8, and owns their
writable headers and extensions. Failed initialization removes these objects
without freeing immutable glyph storage. Both inputs are included in the
standalone project; neither a ROM nor a sibling project is a build dependency.
The remaining vector/queue startup guards still prevent publication of the
unfinished library.
