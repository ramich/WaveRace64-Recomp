#ifndef WR64_REGISTER_PATCHES_H
#define WR64_REGISTER_PATCHES_H

namespace wr64 {

// Registers the recompiled game patches (RecompiledPatches/) with librecomp.
// Must be called before recomp::start().
void register_patches();

} // namespace wr64

#endif // WR64_REGISTER_PATCHES_H
