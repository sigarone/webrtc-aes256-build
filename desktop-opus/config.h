/* Build configuration for the vendored libopus, desktop-opus addon.
 *
 * OURS, not upstream's — upstream generates this with autoconf, which this
 * build does not run (same reasoning as the private qaudion-desktop repo's
 * native/opus/config.h, which this file is a derivative of).
 *
 * Pinned Opus source commit: 55513e81d8f606bd75d0ff773d2144e5f2a732f5
 * (https://github.com/xiph/opus, fetched by desktop-opus/fetch-opus-source.sh).
 * This is deliberately NOT the newest tagged release (v1.6.1) — it is the
 * EXACT commit the phone builds already use (m150/README.md's pin table,
 * `third_party/opus/src` = 55513e81d8f606bd75d0ff773d2144e5f2a732f5), so the
 * desktop addon this repo builds conceals and enhances audio identically to
 * Android/iOS instead of merely "also having OSCE" from a different revision
 * with possibly different model weights or codec behavior. If that pin ever
 * moves for the phones, this one should move with it — see
 * desktop-opus/PINS.md.
 */
#ifndef QAUDION_OPUS_CONFIG_H
#define QAUDION_OPUS_CONFIG_H

#define CONFIG_H
#define PACKAGE_VERSION "1.6-qaudion-osce-55513e8"

#define HAVE_LRINTF 1
#define HAVE_LRINT 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_MEMORY_H 1

/* Float build — matches the private repo's native/opus/config.h (desktop is
 * not power-constrained; float path is faster and closer to reference on
 * x86-64). */
#define FLOATING_POINT 1
#define FLOAT_APPROX 1

/* MSVC has no C99 VLAs but does have alloca (malloc.h); everyone else gets
 * VAR_ARRAYS. Identical reasoning to the private repo's config.h. */
#ifdef _MSC_VER
#  define USE_ALLOCA 1
#else
#  define VAR_ARRAYS 1
#endif

#define ENABLE_HARDENING 1

/* No runtime CPU dispatch — same reasoning as the private repo: plain-C DNN
 * and codec kernels, one build description across MSVC/clang/gcc. */
/* #undef OPUS_HAVE_RTCD */

/* Deep PLC (FARGAN). Identical semantics to the private repo's addon:
 * compiled in here; the DECODER must still be set to complexity >= 5 at
 * runtime (opus_addon.cc) for it to engage. */
#define ENABLE_DEEP_PLC 1

/* OSCE (LACE / NoLACE) — the reason this addon exists. Compiled in as
 * arm64/x64-agnostic plain C (see the "No runtime CPU dispatch" note above);
 * engages at runtime when the decoder is set to complexity 6 (LACE) or 7
 * (NoLACE) — see opus_addon.cc. Macro name is xiph/opus's own
 * (configure.ac --enable-osce; CMakeLists.txt OPUS_OSCE), matched exactly to
 * the phone build's own patch (m150/patches/P4a-opus-dnn-osce-build.patch),
 * which is also where DISABLE_DEBUG_FLOAT below is explained. No OSCE-BWE:
 * per P4a's own verification, opus upstream 55513e81 has no bbwenet_* symbol
 * yet, and the pinned model tarball does not contain one either. */
#define ENABLE_OSCE 1

/* Without this, every dnn/*_data.c ALSO compiles a float copy of each int8
 * weight matrix and nnet.c runs the float "debug" inference path: ~4x the
 * weight footprint and a slower, non-default path the OSCE/deep-PLC
 * operating point was never measured with. All three upstream build systems
 * (configure.ac, CMakeLists.txt, meson) default this OFF; defining it
 * explicitly here means node-gyp's hand-rolled build (no configure/cmake/
 * meson to default it) does not silently pick the debug path instead. */
#define DISABLE_DEBUG_FLOAT 1

#endif /* QAUDION_OPUS_CONFIG_H */
