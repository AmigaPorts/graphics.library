# Public graphics headers and interface

`include/graphics/` provides the classic m68k AmigaOS graphics data types and
constants. The implementation uses these same headers. Other OS headers (Exec,
Utility, etc.) remain toolchain dependencies. Graphics call bindings are generated
from this project's `sfd/graphics_lib.sfd`.
`include/internal/` contains private implementation declarations and is not
installed with the developer headers.

The public headers derived from AROS retain their original copyright notices
and are licensed under APL 1.1, reproduced in `LICENSE.APL`. Source:
https://github.com/aros-development-team/AROS/tree/3793aefc6f2d8498b9629eec852e1613cf69f21f/compiler/include/graphics

Adapted on 2026-10-10. Local adaptations preserve the classic m68k ABI instead of AROS host-pointer
sizes and extensions: classic reserved fields and layer layouts, the V47
GfxBase scratch fields, and the classic ExtSprite size. Non-classic interfaces
are not advertised. These declarations do not implement or emulate the Amiga
chipset. Original AmigaOS NDK headers are not included in this distribution.

Changes to these APL-covered files remain under APL 1.1; this does not change
the MPL 2.0 license of the independently implemented library sources.

## Function definitions

`sfd/graphics_lib.sfd` is derived from AROS's `rom/graphics/graphics.conf` at
revision `3793aefc6f2d8498b9629eec852e1613cf69f21f`, under APL 1.1:
https://github.com/aros-development-team/AROS/blob/3793aefc6f2d8498b9629eec852e1613cf69f21f/rom/graphics/graphics.conf

The 2026-10-10 classic m68k adaptation preserves the complete interface in the
local NDK 3.2 SFD 47.2: 163 public functions, nine private slots and ten tag-call
aliases, through WriteChunkyPixels at -1056. Four standard library lifecycle
vectors precede this list. This is not restricted to the AmigaOS 3.1 API.

AROS's register assignments and vector order are retained for shared public
functions. Classic return/argument widths, signedness, pointer types and const
qualifiers replace AROS-specific declarations. The private slots remain private;
AROS display-driver extensions at -642/-648 and functions beyond -1056 are not
part of the classic interface. The classic private declarations and source tag
aliases are supplied too. Alias entries do not consume vector slots.
