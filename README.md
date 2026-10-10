# Open Source reimplementation of `graphics.library` for AmigaOS 3.x
[![CMake](https://github.com/MBeijer/graphics.library/actions/workflows/cmake.yml/badge.svg)](https://github.com/MBeijer/graphics.library/actions/workflows/cmake.yml)

## What?

The `graphics.library` reimplementation is behaviorally based on AmigaOS 3.2.3.
It is intended as a complete open-source replacement for the library in
Kickstart, preserving its public API and behavior, including compatibility with
Picasso96.

## Implementation order

1. Support Emu68's existing framebuffer through `devicetree.resource`.
2. Support the real Amiga chipset on non-Emu68 hardware or when
   `devicetree.resource` is absent.
3. Optimize once functional compatibility and backend integration are in place.

Shared graphics and OS behavior remains independent of the hardware backend.
The framebuffer backend uses the mode already configured by Emu68; the chipset
backend will access real Amiga hardware.

Backend selection must also be available at compile time. A chipset-only ROM
build must omit the Emu68 framebuffer implementation and its device-tree SDK
dependency, while retaining the complete shared graphics API and ABI. Runtime
selection is useful for a combined build but does not meet the ROM-size
requirement on its own. Track linked module size for each configuration. The
chipset-only configuration is planned with the real chipset backend in step 2;
it is not implemented by the current framebuffer build.

## Why?

To liberate AmigaOS and open source it under a fair license, piece by piece

## Building

```bash
cmake -B./build -S./ -DM68K_CPU=custom -DM68K_FPU=custom -DM68K_CRT=custom -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX:PATH=./install
cmake --build ./build -j$(nproc)
cmake --build ./build --target install
```

## Conformance testing

Use [VolAmos's real-library loading](https://github.com/sidick/volamos/pull/126)
for headless API and guest-memory checks where its execution environment
supports the required AmigaOS services. Verify that the run loads this actual
68k library rather than falling back to host implementations. The previously
tested revision does not support our direct-init Resident; that loader failure
is a test-tool limitation, not a conformance result. Preserve the library's
normal initialization and report unsupported tests explicitly.

Use [p96cts](https://github.com/codewiz/p96cts) for external rendering
conformance tests. Despite its name, its palette-mode tests exercise
`graphics.library` on native planar screens too. Test bare RastPorts and both
`LAYER` and `CLIP` runs, retaining the matching upstream golden images and
failure reports. Keep these tests alongside comparisons with the original
AmigaOS implementation: p96cts documents some differences between its P96
software reference and native AmigaOS behavior.

The normal p96cts application requires working screen and monitor activation.
The replacement library is still withheld from publication while its API and
native display initialization are incomplete; a run against another installed
`graphics.library` does not validate this replacement. Coverage additions should
be prepared for p96cts upstream, without weakening its comparisons to make this
library pass.
