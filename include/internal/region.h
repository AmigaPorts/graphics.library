/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <graphics/gfxbase.h>
#include <graphics/regions.h>

struct Region* pcNewRegion(struct GfxBase* graphics asm("a6"));
void pcClearRegion(struct Region* region asm("a0"), struct GfxBase* graphics asm("a6"));
void pcDisposeRegion(struct Region* region asm("a0"), struct GfxBase* graphics asm("a6"));
void pcAndRectRegion(struct Region* region asm("a0"), const struct Rectangle* rectangle asm("a1"),
    struct GfxBase* graphics asm("a6"));
BOOL pcClearRectRegion(struct Region* region asm("a0"), const struct Rectangle* rectangle asm("a1"),
    struct GfxBase* graphics asm("a6"));
BOOL pcOrRectRegion(struct Region* region asm("a0"), const struct Rectangle* rectangle asm("a1"),
    struct GfxBase* graphics asm("a6"));

BOOL pcOrRegionRegion(const struct Region* source asm("a0"), struct Region* destination asm("a1"),
    struct GfxBase* graphics asm("a6"));
BOOL pcAndRegionRegion(const struct Region* source asm("a0"), struct Region* destination asm("a1"),
    struct GfxBase* graphics asm("a6"));

BOOL pcXorRectRegion(struct Region* region asm("a0"), const struct Rectangle* rectangle asm("a1"),
    struct GfxBase* graphics asm("a6"));
BOOL pcXorRegionRegion(const struct Region* source asm("a0"), struct Region* destination asm("a1"),
    struct GfxBase* graphics asm("a6"));

// Region headers and rectangle nodes share the original fixed-size cache.
struct RegionRectangle* pcAllocRegionNode(struct GfxBase* graphics);
void pcFreeRegionNode(struct GfxBase* graphics, struct RegionRectangle* node);
void pcFlushRegionPool(struct GfxBase* graphics);
