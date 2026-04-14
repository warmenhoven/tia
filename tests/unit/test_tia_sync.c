/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "tia.h"
#include "test_framework.h"

static void tick_n(struct tia *t, int n) { while (n-- > 0) tia_tick(t); }

/* Run through a full scanline (228 clocks) — brings hpos back to 0
 * and advances scanline by 1 (unless vsync is asserted). */
static void full_scanline(struct tia *t) { tick_n(t, TIA_SCANLINE_CLOCKS); }

/* --- Beam timing --- */

static int test_tick_advances_hpos(void)
{
    struct tia t;
    tia_init(&t);
    ASSERT_EQ(t.hpos, 0);
    tick_n(&t, 5);
    ASSERT_EQ(t.hpos, 5);
    return 0;
}

static int test_scanline_wrap(void)
{
    struct tia t;
    tia_init(&t);
    tick_n(&t, TIA_SCANLINE_CLOCKS - 1);
    ASSERT_EQ(t.hpos, 227);
    ASSERT_EQ(t.scanline, 0);
    tia_tick(&t);
    ASSERT_EQ(t.hpos, 0);
    ASSERT_EQ(t.scanline, 1);
    return 0;
}

/* --- WSYNC --- */

static int test_wsync_asserts_rdy_then_releases_at_scanline_start(void)
{
    struct tia t;
    tia_init(&t);
    tick_n(&t, 100);                       /* beam mid-scanline */
    tia_write(&t, 0x02, 0);                /* WSYNC strobe */
    ASSERT_TRUE(t.rdy_asserted);
    /* Tick to just before end of scanline: still stalled. */
    tick_n(&t, TIA_SCANLINE_CLOCKS - 100 - 1);
    ASSERT_TRUE(t.rdy_asserted);
    ASSERT_EQ(t.hpos, 227);
    /* One more tick wraps the scanline and releases RDY. */
    tia_tick(&t);
    ASSERT_EQ(t.hpos, 0);
    ASSERT_TRUE(!t.rdy_asserted);
    return 0;
}

/* --- Background rendering --- */

static int test_colubk_fills_visible_scanline(void)
{
    struct tia t;
    uint32_t expected;
    int x;
    tia_init(&t);
    tia_write(&t, 0x09, 0x44);             /* COLUBK = hue 4, luma 2 */
    expected = t.palette[(0x44 >> 1) & 0x7F];
    full_scanline(&t);                     /* renders scanline 0 entirely */
    for (x = 0; x < TIA_VISIBLE_WIDTH; x++)
        ASSERT_EQ(t.fb[x], expected);
    return 0;
}

static int test_hblank_pixels_not_written(void)
{
    struct tia t;
    tia_init(&t);
    /* Pre-fill scanline 0 with a sentinel, then tick only through HBLANK. */
    {
        int i;
        for (i = 0; i < TIA_VISIBLE_WIDTH; i++)
            t.fb[i] = 0xDEADBEEF;
    }
    tia_write(&t, 0x09, 0x20);
    tick_n(&t, TIA_HBLANK_CLOCKS);         /* still in HBLANK after these */
    {
        int i;
        for (i = 0; i < TIA_VISIBLE_WIDTH; i++)
            ASSERT_EQ(t.fb[i], 0xDEADBEEF);
    }
    return 0;
}

static int test_vblank_skips_pixel_writes(void)
{
    struct tia t;
    int x;
    tia_init(&t);
    /* Fill scanline 0 with sentinel. */
    for (x = 0; x < TIA_VISIBLE_WIDTH; x++) t.fb[x] = 0xDEADBEEF;
    tia_write(&t, 0x01, 0x02);             /* VBLANK on */
    tia_write(&t, 0x09, 0x50);
    full_scanline(&t);
    for (x = 0; x < TIA_VISIBLE_WIDTH; x++)
        ASSERT_EQ(t.fb[x], 0xDEADBEEF);
    return 0;
}

/* --- VSYNC frame boundary --- */

static int test_vsync_rising_edge_signals_frame(void)
{
    struct tia t;
    tia_init(&t);
    /* Advance a few scanlines */
    tick_n(&t, 3 * TIA_SCANLINE_CLOCKS);
    ASSERT_EQ(t.scanline, 3);
    ASSERT_TRUE(!t.frame_ready);
    ASSERT_EQ(t.frame_number, 0);

    tia_write(&t, 0x00, 0x02);             /* VSYNC rising edge */
    ASSERT_TRUE(t.frame_ready);
    ASSERT_EQ(t.frame_number, 1);
    ASSERT_EQ(t.scanline, 0);

    /* Writing VSYNC=1 again (no rising edge) must not retrigger. */
    tia_write(&t, 0x00, 0x02);
    ASSERT_EQ(t.frame_number, 1);

    /* Clearing VSYNC then setting again triggers a new frame. */
    tia_write(&t, 0x00, 0x00);
    tia_write(&t, 0x00, 0x02);
    ASSERT_EQ(t.frame_number, 2);
    return 0;
}

static int test_vsync_scanline_doesnt_advance(void)
{
    struct tia t;
    tia_init(&t);
    tia_write(&t, 0x00, 0x02);             /* start VSYNC */
    tick_n(&t, 3 * TIA_SCANLINE_CLOCKS);
    ASSERT_EQ(t.scanline, 0);              /* scanline frozen during vsync */
    return 0;
}

/* --- RSYNC --- */

static int test_rsync_aligns_hpos_near_hsync(void)
{
    /* RSYNC aligns the horizontal counter so the current scanline wraps
     * ~3 clocks from now. The internal hcounter is driven to H_CLOCKS-3;
     * 3 more ticks wrap back to hp=0. */
    struct tia t;
    tia_init(&t);
    tick_n(&t, 120);
    ASSERT_EQ(t.hpos, 120);
    tia_write(&t, 0x03, 0);                /* RSYNC strobe */
    ASSERT_EQ(t.hpos, TIA_SCANLINE_CLOCKS - 3);
    tick_n(&t, 3);
    ASSERT_EQ(t.hpos, 0);
    return 0;
}

/* --- Register address mirroring --- */

static int test_colubk_mirror_addresses(void)
{
    struct tia t;
    tia_init(&t);
    tia_write(&t, 0x49, 0x82);             /* $09 | 0x40 mirror */
    ASSERT_EQ(t.colubk, 0x82);
    return 0;
}

/* --- Serialization --- */

static int test_serialize_roundtrip(void)
{
    struct tia a, b;
    uint8_t buf[128];
    size_t sz = tia_serialize_size();

    ASSERT_TRUE(sz <= sizeof(buf));
    tia_init(&a);
    tick_n(&a, 123);
    tia_write(&a, 0x09, 0x5A);
    tia_write(&a, 0x02, 0);                 /* WSYNC */
    tia_write(&a, 0x00, 0x02);              /* VSYNC set -> frame_ready */

    tia_serialize(&a, buf);
    tia_init(&b);
    ASSERT_TRUE(tia_deserialize(&b, buf, sz));
    ASSERT_EQ(b.hpos, a.hpos);
    ASSERT_EQ(b.scanline, a.scanline);
    ASSERT_EQ(b.frame_number, a.frame_number);
    ASSERT_EQ(b.frame_ready, a.frame_ready);
    ASSERT_EQ(b.vsync, a.vsync);
    ASSERT_EQ(b.vblank, a.vblank);
    ASSERT_EQ(b.rdy_asserted, a.rdy_asserted);
    ASSERT_EQ(b.colubk, a.colubk);
    return 0;
}

static int test_deserialize_rejects_short_buffer(void)
{
    struct tia t;
    uint8_t buf[128];
    tia_init(&t);
    ASSERT_TRUE(!tia_deserialize(&t, buf, tia_serialize_size() - 1));
    return 0;
}

TEST_MAIN_BEGIN
    RUN_TEST(test_tick_advances_hpos);
    RUN_TEST(test_scanline_wrap);
    RUN_TEST(test_wsync_asserts_rdy_then_releases_at_scanline_start);
    RUN_TEST(test_colubk_fills_visible_scanline);
    RUN_TEST(test_hblank_pixels_not_written);
    RUN_TEST(test_vblank_skips_pixel_writes);
    RUN_TEST(test_vsync_rising_edge_signals_frame);
    RUN_TEST(test_vsync_scanline_doesnt_advance);
    RUN_TEST(test_rsync_aligns_hpos_near_hsync);
    RUN_TEST(test_colubk_mirror_addresses);
    RUN_TEST(test_serialize_roundtrip);
    RUN_TEST(test_deserialize_rejects_short_buffer);
TEST_MAIN_END
