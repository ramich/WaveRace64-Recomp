#ifndef WR64_PATCHES_H
#define WR64_PATCHES_H

// Section attributes understood by N64Recomp's patch recompilation
// (same conventions as Zelda64Recomp / Pilotwings64Recomp).
#define RECOMP_EXPORT __attribute__((section(".recomp_export")))
#define RECOMP_PATCH __attribute__((section(".recomp_patch")))
#define RECOMP_FORCE_PATCH __attribute__((section(".recomp_force_patch")))

// Basic N64 types for patch code (no decomp headers available yet).
typedef signed char s8;
typedef unsigned char u8;
typedef signed short s16;
typedef unsigned short u16;
typedef signed int s32;
typedef unsigned int u32;
typedef signed long long s64;
typedef unsigned long long u64;
typedef float f32;
typedef double f64;

#endif // WR64_PATCHES_H
