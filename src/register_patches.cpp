/**
 * @file register_patches.cpp
 * @brief Registers the recompiled game patches with librecomp.
 *
 * The patches pipeline: patches/*.c are cross-compiled to MIPS
 * (patches/Makefile), linked into patches.elf, recompiled to native code by
 * N64Recomp (patches.toml -> RecompiledPatches/), and registered here so
 * librecomp swaps them in over the original game functions at load time.
 */

#include "register_patches.h"

#include "../RecompiledPatches/patches_bin.h"
#include "../RecompiledPatches/recomp_overlays.inl"

#include "librecomp/overlays.hpp"
#include "librecomp/game.hpp"

namespace wr64 {

void register_patches() {
    recomp::overlays::register_patches(mm_patches_bin, sizeof(mm_patches_bin), section_table, ARRLEN(section_table));
    recomp::overlays::register_base_exports(export_table);
    recomp::overlays::register_base_events(event_names);
    recomp::overlays::register_manual_patch_symbols(manual_patch_symbols);
}

} // namespace wr64
