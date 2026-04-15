/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Doc-anchored RES*x pipeline tests.
 *
 * Sources:
 *   SPG §RESPx — "A strobe to RESPx resets the horizontal-position counter
 *     for the corresponding player/missile/ball object."
 *   Towers "TIA Hardware Analysis" — strobe pipeline:
 *     - Strobe during visible area: object rendering begins 5 color clocks
 *       after the strobe for PLAYERS (RESP0/RESP1). Missiles and ball
 *       (RESM0/1, RESBL) have a shorter 4-clock pipeline.
 *     - Strobe during HBLANK: shorter pipeline still; sprite appears
 *       earlier in the next visible region (model simplification: we don't
 *       fully emulate HBLANK variants; this is a known gap).
 *
 * The test strategy for each RES*x:
 *   1. Set up sprite graphics (single-bit GRP / enabled ball/missile)
 *   2. Tick to a known visible hpos
 *   3. Strobe RES*
 *   4. Tick to end of scanline
 *   5. Assert the sprite's first visible pixel is at (strobe_hpos + delay - 68)
 *
 * Where delay = 5 for player, 4 for missile/ball (per Towers).
 *
 * Two concrete test anchors from Adventure, where the Player vs Ball
 * pipeline delta is visible to the eye:
 *   - The yellow-castle gate (Player 0) is at p0_pos=76
 *   - The in-castle player character (Ball) is at bl_pos=78, right edge
 *     1 pixel left of the gate's right edge
 * These anchor the ±1-clock distinction between Player and Ball pipelines.
 */

#include <stdio.h>
#include <string.h>
#include "tia.h"
#include "test_framework.h"

static void full_scanline(struct tia *t)
{
    int i;
    for (i = 0; i < TIA_SCANLINE_CLOCKS; i++) tia_tick(t);
}

#define BG(t)  ((t).palette[((t).colubk >> 1) & 0x7F])
#define P0(t)  ((t).palette[((t).colup0 >> 1) & 0x7F])
#define P1(t)  ((t).palette[((t).colup1 >> 1) & 0x7F])
#define PF(t)  ((t).palette[((t).colupf >> 1) & 0x7F])

static void setup(struct tia *t)
{
    tia_init(t);
    tia_write(t, 0x09, 0x44);
    tia_write(t, 0x06, 0x82);
    tia_write(t, 0x07, 0x24);
    tia_write(t, 0x08, 0x66);
}

/* Tick to the specified hpos and strobe addr. */
static void tick_to_hpos_and_strobe(struct tia *t, int hpos, uint16_t strobe_addr)
{
    int i;
    for (i = 0; i < hpos; i++) tia_tick(t);
    tia_write(t, strobe_addr, 0);
}

/* --- Player pipeline: strobe during visible area, 5-clock latency ---
 *
 * Towers: "RESPx during visible (hctr >= 68): the decode pipeline produces
 * the first visible pixel at strobe_hpos + 5". */

static int test_resp0_visible_hpos_100_yields_pixel_at_37(void)
{
    /* Strobe at hpos=100. First rendered pixel at visible-x = 100 + 5 - 68 = 37. */
    struct tia t;
    setup(&t);
    tia_write(&t, 0x1B, 0x80);            /* GRP0 bit 7 = leftmost pixel on */
    tick_to_hpos_and_strobe(&t, 100, 0x10);
    full_scanline(&t);
    ASSERT_EQ(t.fb[37], P0(t));
    ASSERT_EQ(t.fb[36], BG(t));
    ASSERT_EQ(t.fb[38], BG(t));   /* only bit 7 on → single pixel */
    return 0;
}

static int test_resp0_visible_hpos_150_yields_pixel_at_87(void)
{
    struct tia t;
    setup(&t);
    tia_write(&t, 0x1B, 0x80);
    tick_to_hpos_and_strobe(&t, 150, 0x10);
    full_scanline(&t);
    ASSERT_EQ(t.fb[87], P0(t));
    ASSERT_EQ(t.fb[86], BG(t));
    return 0;
}

static int test_resp0_visible_hpos_68_yields_pixel_at_5(void)
{
    /* Earliest "visible" strobe is at hpos = HBLANK end = 68. 68+5-68 = 5. */
    struct tia t;
    setup(&t);
    tia_write(&t, 0x1B, 0x80);
    tick_to_hpos_and_strobe(&t, 68, 0x10);
    full_scanline(&t);
    ASSERT_EQ(t.fb[5], P0(t));
    ASSERT_EQ(t.fb[4], BG(t));
    return 0;
}

static int test_resp1_visible_hpos_100_yields_pixel_at_37(void)
{
    struct tia t;
    setup(&t);
    tia_write(&t, 0x1C, 0x80);
    tick_to_hpos_and_strobe(&t, 100, 0x11);
    full_scanline(&t);
    ASSERT_EQ(t.fb[37], P1(t));
    return 0;
}

/* --- Missile pipeline: 4-clock latency ---
 *
 * Towers: "RESMx during visible: the missile decode uses a 4-clock pipeline,
 * so first pixel at strobe_hpos + 4 - HBLANK." */

static int test_resm0_visible_hpos_100_yields_pixel_at_36(void)
{
    struct tia t;
    setup(&t);
    tia_write(&t, 0x04, 0x00);           /* missile size 1 */
    tia_write(&t, 0x1D, 0x02);           /* ENAM0 */
    tick_to_hpos_and_strobe(&t, 100, 0x12);
    full_scanline(&t);
    ASSERT_EQ(t.fb[36], P0(t));
    ASSERT_EQ(t.fb[35], BG(t));
    ASSERT_EQ(t.fb[37], BG(t));
    return 0;
}

static int test_resm1_visible_hpos_100_yields_pixel_at_36(void)
{
    struct tia t;
    setup(&t);
    tia_write(&t, 0x05, 0x00);
    tia_write(&t, 0x1E, 0x02);
    tick_to_hpos_and_strobe(&t, 100, 0x13);
    full_scanline(&t);
    ASSERT_EQ(t.fb[36], P1(t));
    return 0;
}

/* --- Ball pipeline: 4-clock latency, COLUPF ---
 *
 * Towers: "RESBL is identical in pipeline to RESMx — 4-clock decode. Ball
 * uses COLUPF as its colour source." */

static int test_resbl_visible_hpos_100_yields_pixel_at_36(void)
{
    struct tia t;
    setup(&t);
    tia_write(&t, 0x0A, 0x00);           /* ball size 1 */
    tia_write(&t, 0x1F, 0x02);           /* ENABL */
    tick_to_hpos_and_strobe(&t, 100, 0x14);
    full_scanline(&t);
    ASSERT_EQ(t.fb[36], PF(t));
    ASSERT_EQ(t.fb[35], BG(t));
    ASSERT_EQ(t.fb[37], BG(t));
    return 0;
}

/* --- Player vs Ball pipeline difference (visible in Adventure) ---
 *
 * On real hardware, strobing RESP0 and RESBL at the same hpos on the
 * same scanline produces sprites whose first-pixel positions differ by
 * exactly 1 (player rightward of ball by 1 color clock, due to the
 * 5-clock vs 4-clock pipeline delta). Adventure's yellow castle + player
 * character relies on this. If RESP and RESBL offsets get re-tuned to
 * the same value, Adventure's gate and player character will align
 * incorrectly. */

static int test_resp0_is_one_pixel_right_of_resbl_at_same_hpos(void)
{
    struct tia a, b;
    setup(&a);
    setup(&b);
    /* Sprite A: single-pixel player */
    tia_write(&a, 0x1B, 0x80);
    tick_to_hpos_and_strobe(&a, 100, 0x10);
    full_scanline(&a);
    full_scanline(&a);                  /* complete scanline 1 render */
    /* Sprite B: single-pixel ball at the same strobe hpos */
    tia_write(&b, 0x0A, 0x00);
    tia_write(&b, 0x1F, 0x02);
    tick_to_hpos_and_strobe(&b, 100, 0x14);
    full_scanline(&b);
    full_scanline(&b);

    /* After strobe at scanline-0 hpos=100 + 2 * full_scanline, scanline 1
     * has been rendered for its full 228 clocks. Check that row. */
    {
        int ai, bi, afound = -1, bfound = -1;
        size_t row1 = 1 * TIA_VISIBLE_WIDTH;
        for (ai = 0; ai < TIA_VISIBLE_WIDTH; ai++)
            if (a.fb[row1 + ai] != BG(a)) { afound = ai; break; }
        for (bi = 0; bi < TIA_VISIBLE_WIDTH; bi++)
            if (b.fb[row1 + bi] != BG(b)) { bfound = bi; break; }
        if (afound < 0 || bfound < 0) {
            fprintf(stderr, "sprite missing: p0@%d bl@%d\n", afound, bfound);
            return 1;
        }
        if (afound - bfound != 1) {
            fprintf(stderr, "pipeline delta wrong: p0@%d bl@%d (Δ=%d, want 1)\n",
                    afound, bfound, afound - bfound);
            return 1;
        }
    }
    return 0;
}

/* --- Strobing during HBLANK ---
 *
 * Per Towers, the RES* pipeline delay depends on where in the scanline
 * the strobe fires (three distinct counter values: hblank / lateHblank /
 * frame). Our current model uses a single fixed offset for the whole
 * scanline; this test captures the *current* behaviour as a regression
 * guard — if we later add proper HBLANK pipeline modeling, update the
 * expected value to the Towers-documented behaviour.
 *
 * Regression guard: strobing RESP0 at hpos=30 (deep HBLANK) should place
 * the first sprite pixel at a negative "position" (before visible area),
 * so no sprite pixel should appear early in the visible scanline — this
 * is a functional smoke test, not an anchored-to-docs value. */

static int test_resp0_during_hblank_produces_negative_position(void)
{
    struct tia t;
    int x;
    setup(&t);
    tia_write(&t, 0x1B, 0x80);
    tick_to_hpos_and_strobe(&t, 30, 0x10);
    full_scanline(&t);
    /* Sprite position is negative, so within our simple model it doesn't
     * render in the visible area. Check no P0_COLOR pixel appears. */
    for (x = 0; x < TIA_VISIBLE_WIDTH; x++) {
        if (t.fb[x] == P0(t)) {
            fprintf(stderr, "HBLANK strobe produced visible pixel at x=%d\n", x);
            return 1;
        }
    }
    return 0;
}

TEST_MAIN_BEGIN
    RUN_TEST(test_resp0_visible_hpos_100_yields_pixel_at_37);
    RUN_TEST(test_resp0_visible_hpos_150_yields_pixel_at_87);
    RUN_TEST(test_resp0_visible_hpos_68_yields_pixel_at_5);
    RUN_TEST(test_resp1_visible_hpos_100_yields_pixel_at_37);
    RUN_TEST(test_resm0_visible_hpos_100_yields_pixel_at_36);
    RUN_TEST(test_resm1_visible_hpos_100_yields_pixel_at_36);
    RUN_TEST(test_resbl_visible_hpos_100_yields_pixel_at_36);
    RUN_TEST(test_resp0_is_one_pixel_right_of_resbl_at_same_hpos);
    RUN_TEST(test_resp0_during_hblank_produces_negative_position);
TEST_MAIN_END
