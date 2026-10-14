# Public headers and interface

`include/graphics/` provides classic m68k AmigaOS types and constants.
`sfd/graphics_lib.sfd` generates the call bindings. Private declarations stay
in `include/internal/` and are not installed.

The SFD covers the AmigaOS 3.2 interface through `WriteChunkyPixels` at -1056:
163 public functions, nine private slots and ten tag-call aliases, following
the four library lifecycle vectors. Aliases consume no additional vectors.

Headers and SFD derive from AROS's
[graphics headers](https://github.com/aros-development-team/AROS/tree/3793aefc6f2d8498b9629eec852e1613cf69f21f/compiler/include/graphics) and
[`graphics.conf`](https://github.com/aros-development-team/AROS/blob/3793aefc6f2d8498b9629eec852e1613cf69f21f/rom/graphics/graphics.conf)
at revision `3793aefc6f2d8498b9629eec852e1613cf69f21f`, adapted to the classic
m68k ABI and licensed under APL-1.1. Original copyright notices remain in the
files; see `LICENSE.APL`. AmigaOS NDK headers are not redistributed.

Installation places public headers, generated bindings and the SFD under
`Developer/`. Other Amiga SDK headers, such as Exec and Utility, remain required.
