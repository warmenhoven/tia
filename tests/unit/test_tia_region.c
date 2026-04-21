/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Region handling: palette selection, auto-detect classifier, and
 * serialize round-trip. The detect classifier only distinguishes NTSC
 * vs PAL by scanline count; PAL60/SECAM are forced via the libretro
 * core option and are never auto-selected. */

#include <string.h>
#include "tia.h"
#include "test_framework.h"

/* Simulate a full frame boundary by placing the TIA at `scanlines` and
 * toggling VSYNC low→high→low. That's exactly what real ROMs do on each
 * VSYNC and what the detect code samples. */
static void simulate_frame(struct tia *t, uint16_t scanlines)
{
    t->scanline = scanlines;
    tia_write(t, 0x00, 0x00);
    tia_write(t, 0x00, 0x02);   /* rising edge: sample + increment frame */
    tia_write(t, 0x00, 0x00);
}

/* --- Palette selection --- */

static int test_set_region_swaps_palette(void)
{
    struct tia t;
    tia_init(&t);
    ASSERT_TRUE(t.palette == tia_ntsc_palette);
    tia_set_region(&t, TIA_REGION_PAL);
    ASSERT_TRUE(t.palette == tia_pal_palette);
    tia_set_region(&t, TIA_REGION_SECAM);
    ASSERT_TRUE(t.palette == tia_secam_palette);
    tia_set_region(&t, TIA_REGION_NTSC);
    ASSERT_TRUE(t.palette == tia_ntsc_palette);
    return 0;
}

static int test_pal60_uses_pal_palette(void)
{
    /* PAL60 is PAL colours at NTSC timing. Timing is libretro's concern;
     * from the TIA's view, the only observable difference is the palette. */
    struct tia t;
    tia_init(&t);
    tia_set_region(&t, TIA_REGION_PAL60);
    ASSERT_TRUE(t.palette == tia_pal_palette);
    return 0;
}

static int test_secam_palette_has_8_unique_colors(void)
{
    /* SECAM on 2600 hardware only had 8 distinct colours (luma-only).
     * Our 128-entry palette replicates the same 8 across every hue. */
    int hue, luma;
    for (hue = 0; hue < 16; hue++)
        for (luma = 0; luma < 8; luma++)
            ASSERT_EQ(tia_secam_palette[hue * 8 + luma],
                      tia_secam_palette[luma]);
    return 0;
}

/* --- Palette variant (standard vs z26) --- */

static int test_palette_variant_swaps_tables(void)
{
    struct tia t;
    tia_init(&t);
    /* Default is standard. */
    ASSERT_TRUE(t.palette == tia_ntsc_palette);
    tia_set_palette_variant(&t, TIA_PALETTE_Z26);
    ASSERT_TRUE(t.palette == tia_ntsc_palette_z26);
    /* Region change carries the variant along. */
    tia_set_region(&t, TIA_REGION_PAL);
    ASSERT_TRUE(t.palette == tia_pal_palette_z26);
    tia_set_region(&t, TIA_REGION_SECAM);
    ASSERT_TRUE(t.palette == tia_secam_palette_z26);
    /* And flipping back to standard re-selects the standard table. */
    tia_set_palette_variant(&t, TIA_PALETTE_STANDARD);
    ASSERT_TRUE(t.palette == tia_secam_palette);
    return 0;
}

/* --- Auto-detect classifier --- */

static int test_detect_ntsc_on_262_line_frames(void)
{
    struct tia t;
    int i;
    tia_init(&t);
    /* 6 frames: frame 0 skipped, frames 1..5 sampled → lock. */
    for (i = 0; i < 6; i++) simulate_frame(&t, 262);
    ASSERT_TRUE(t.detect_locked);
    ASSERT_EQ(t.detected_region, TIA_REGION_NTSC);
    return 0;
}

static int test_detect_pal_on_312_line_frames(void)
{
    struct tia t;
    int i;
    tia_init(&t);
    for (i = 0; i < 6; i++) simulate_frame(&t, 312);
    ASSERT_TRUE(t.detect_locked);
    ASSERT_EQ(t.detected_region, TIA_REGION_PAL);
    return 0;
}

static int test_detect_majority_wins(void)
{
    /* If 3 of 5 samples are PAL-length, lock on PAL. Mixed sample streams
     * happen in real ROMs that miscount VSYNC during their boot frames. */
    struct tia t;
    tia_init(&t);
    simulate_frame(&t, 262); /* frame 0 skipped */
    simulate_frame(&t, 262); /* NTSC sample */
    simulate_frame(&t, 312); /* PAL */
    simulate_frame(&t, 312); /* PAL */
    simulate_frame(&t, 262); /* NTSC */
    simulate_frame(&t, 312); /* PAL → locks: 3 PAL / 2 NTSC */
    ASSERT_TRUE(t.detect_locked);
    ASSERT_EQ(t.detected_region, TIA_REGION_PAL);
    return 0;
}

static int test_detect_does_not_lock_before_five_samples(void)
{
    struct tia t;
    int i;
    tia_init(&t);
    /* 5 frames = frame 0 skipped + 4 samples, short of the 5-sample lock. */
    for (i = 0; i < 5; i++) simulate_frame(&t, 312);
    ASSERT_TRUE(!t.detect_locked);
    return 0;
}

/* --- Serialize round-trip --- */

static int test_serialize_preserves_region(void)
{
    struct tia a, b;
    uint8_t buf[256];
    size_t sz = tia_serialize_size();
    int i;
    tia_init(&a);
    tia_set_region(&a, TIA_REGION_PAL);
    for (i = 0; i < 6; i++) simulate_frame(&a, 312);
    ASSERT_TRUE(a.detect_locked);

    tia_serialize(&a, buf);
    tia_init(&b);
    ASSERT_TRUE(tia_deserialize(&b, buf, sz));
    ASSERT_EQ(b.region, a.region);
    ASSERT_EQ(b.detected_region, a.detected_region);
    ASSERT_TRUE(b.detect_locked);
    ASSERT_TRUE(b.palette == tia_pal_palette);
    return 0;
}

TEST_MAIN_BEGIN
    RUN_TEST(test_set_region_swaps_palette);
    RUN_TEST(test_pal60_uses_pal_palette);
    RUN_TEST(test_secam_palette_has_8_unique_colors);
    RUN_TEST(test_palette_variant_swaps_tables);
    RUN_TEST(test_detect_ntsc_on_262_line_frames);
    RUN_TEST(test_detect_pal_on_312_line_frames);
    RUN_TEST(test_detect_majority_wins);
    RUN_TEST(test_detect_does_not_lock_before_five_samples);
    RUN_TEST(test_serialize_preserves_region);
TEST_MAIN_END
