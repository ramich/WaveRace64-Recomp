#include "patches.h"

// Placeholder so the patches ELF has a non-empty .text section while the
// pipeline is validated. Real patches (border removal, HUD aspect, edge
// culling, eventually 60fps) replace this. Not a RECOMP_PATCH: it patches
// nothing and is never called.
u32 wr64_patches_placeholder(u32 x) {
    return x + 1;
}
