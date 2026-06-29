/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * TIA palette tables.  128 entries each; index with (colu_reg >> 1) & 0x7F.
 * Shared by the behavioral and Verilog TIA backends. */

#ifndef TIA_PALETTE_H
#define TIA_PALETTE_H

#include <stdint.h>

extern const uint32_t tia_ntsc_palette[128];
extern const uint32_t tia_pal_palette[128];
extern const uint32_t tia_secam_palette[128];
extern const uint32_t tia_ntsc_palette_z26[128];
extern const uint32_t tia_pal_palette_z26[128];
extern const uint32_t tia_secam_palette_z26[128];

#endif
