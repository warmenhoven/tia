/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef TIA_COMPAT_H
#define TIA_COMPAT_H

/* Thin compatibility shim for MSVC toolchains older than VS2013, which
 * predate C99's <stdbool.h>. Matches the shim libretro.h uses so `bool`
 * has the same width/signedness across our code and the libretro API.
 * All other compilers (gcc, clang, VS2013+) pick up the real header.
 *
 * <stdint.h> is NOT shimmed here — libretro.h requires it unconditionally
 * at the top of its own file, so it's a hard prerequisite of the build.
 * MSVC 2005 users have to supply a stdint.h via their include path. */

#include <stdint.h>

#if defined(_MSC_VER) && _MSC_VER < 1800 && !defined(__cplusplus)
#define bool  unsigned char
#define true  1
#define false 0
#else
#include <stdbool.h>
#endif

#endif
