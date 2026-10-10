/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "framebuffer.h"

// Private clients of the public devicetree.resource API. No sibling project
// sources or private resource structures are needed by the standalone build.
APTR pcGraphicsFindCompatible(APTR tree_base, APTR root, APTR previous, const char* compatible);
BOOL pcReadGraphicsFramebuffer(APTR tree_base, APTR key, struct FramebufferMode* mode);
