/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <string.h>
#include "tia.h"
#include "test_framework.h"

static void tick_n(struct tia *t, int n)
{
    while (n-- > 0) tia_tick(t);
}

static void scanlines(struct tia *t, int n)
{
    tick_n(t, n * TIA_SCANLINE_CLOCKS);
}

/* --- Register writes mask correctly --- */

static int test_audc_masks_to_4_bits(void)
{
    struct tia t;
    tia_init(&t);
    tia_write(&t, 0x15, 0xF5);        /* AUDC0 = 0xF5 -> only low nibble used */
    ASSERT_EQ(t.aud[0].audc, 0x05);
    return 0;
}

static int test_audf_masks_to_5_bits(void)
{
    struct tia t;
    tia_init(&t);
    tia_write(&t, 0x17, 0xFF);
    ASSERT_EQ(t.aud[0].audf, 0x1F);
    return 0;
}

static int test_audv_masks_to_4_bits(void)
{
    struct tia t;
    tia_init(&t);
    tia_write(&t, 0x19, 0xA7);
    ASSERT_EQ(t.aud[0].audv, 0x07);
    return 0;
}

/* --- Sample rate: 2 samples per scanline --- */

static int test_two_samples_per_scanline(void)
{
    struct tia t;
    int16_t buf[32];
    size_t n;
    tia_init(&t);
    scanlines(&t, 5);
    n = tia_drain_audio(&t, buf, sizeof(buf)/sizeof(buf[0]));
    ASSERT_EQ(n, 10);
    return 0;
}

/* --- Silent channel: AUDV=0 gives zero samples --- */

static int test_silence_with_audv_zero(void)
{
    struct tia t;
    int16_t buf[32];
    size_t n;
    size_t i;
    tia_init(&t);
    tia_write(&t, 0x15, 0x04);        /* AUDC0 = pure tone */
    tia_write(&t, 0x19, 0x00);        /* AUDV0 = 0 */
    scanlines(&t, 10);
    n = tia_drain_audio(&t, buf, sizeof(buf)/sizeof(buf[0]));
    ASSERT_TRUE(n > 0);
    for (i = 0; i < n; i++) ASSERT_EQ(buf[i], 0);
    return 0;
}

/* --- AUDC=0 floods registers, producing constant DC at AUDV --- */

static int test_audc_zero_produces_dc(void)
{
    struct tia t;
    int16_t buf[32];
    size_t n;
    size_t i;
    tia_init(&t);
    tia_write(&t, 0x15, 0x00);        /* AUDC0 = silence mode */
    tia_write(&t, 0x19, 0x08);        /* AUDV0 = 8 */
    scanlines(&t, 10);
    n = tia_drain_audio(&t, buf, sizeof(buf)/sizeof(buf[0]));
    /* After initial transient (~4 samples), output stabilizes at mix[8]. */
    ASSERT_TRUE(n >= 10);
    for (i = 6; i < n; i++) {
        if (buf[i] != t.audio_mix[8]) {
            fprintf(stderr, "sample %zu: expected %d got %d\n",
                    i, t.audio_mix[8], buf[i]);
            return 1;
        }
    }
    return 0;
}

/* --- Pure tone (AUDC=4, AUDF=0) alternates samples between 0 and mix[v] --- */

static int test_audc_pure_tone_alternates(void)
{
    struct tia t;
    int16_t buf[32];
    size_t n;
    size_t i;
    tia_init(&t);
    tia_write(&t, 0x15, 0x04);        /* AUDC0 = PURE (div-2) */
    tia_write(&t, 0x17, 0x00);        /* AUDF0 = 0 (divide by 1) */
    tia_write(&t, 0x19, 0x08);        /* AUDV0 = 8 */
    scanlines(&t, 10);
    n = tia_drain_audio(&t, buf, sizeof(buf)/sizeof(buf[0]));
    /* After settling, pattern is [0, mix[8], 0, mix[8], ...]. Skip initial
     * settle samples and verify alternation on the last N samples. */
    ASSERT_TRUE(n >= 10);
    for (i = 4; i + 1 < n; i += 2) {
        int16_t a = buf[i], b = buf[i + 1];
        if (!((a == 0 && b == t.audio_mix[8]) ||
              (a == t.audio_mix[8] && b == 0))) {
            fprintf(stderr, "pair %zu: got {%d, %d}, expected 0/mix[8]=%d\n",
                    i, a, b, t.audio_mix[8]);
            return 1;
        }
    }
    return 0;
}

/* --- Drain returns queued samples and shifts remainder --- */

static int test_drain_partial(void)
{
    struct tia t;
    int16_t buf[4];
    size_t n;
    tia_init(&t);
    scanlines(&t, 5);                 /* queues 10 samples */
    n = tia_drain_audio(&t, buf, 4);
    ASSERT_EQ(n, 4);
    ASSERT_EQ(t.audio_buf_len, 6);
    n = tia_drain_audio(&t, buf, 4);
    ASSERT_EQ(n, 4);
    ASSERT_EQ(t.audio_buf_len, 2);
    n = tia_drain_audio(&t, buf, 4);
    ASSERT_EQ(n, 2);
    ASSERT_EQ(t.audio_buf_len, 0);
    n = tia_drain_audio(&t, buf, 4);
    ASSERT_EQ(n, 0);
    return 0;
}

/* --- AUDF change mid-tone does NOT reset divider --- */

static int test_audf_change_preserves_divider(void)
{
    struct tia t;
    tia_init(&t);
    tia_write(&t, 0x17, 0x10);
    /* Tick enough to advance div_counter partway. */
    tick_n(&t, 10);
    {
        uint8_t div_before = t.aud[0].div_counter;
        tia_write(&t, 0x17, 0x05);
        ASSERT_EQ(t.aud[0].div_counter, div_before);
    }
    return 0;
}

/* --- Mixing table nonlinearity: dual-channel max ≠ 2× single-channel max --- */

static int test_mix_table_compression(void)
{
    struct tia t;
    tia_init(&t);
    /* mix[30] is max (0x7FFF); mix[15] should be >0x7FFF/2 due to compression. */
    ASSERT_EQ(t.audio_mix[30], 32767);
    ASSERT_EQ(t.audio_mix[0], 0);
    ASSERT_TRUE(t.audio_mix[15] > 16384);
    /* Dual-channel max — if linear, mix[30] would be 2*mix[15]; compression
     * makes it less than that. */
    ASSERT_TRUE(t.audio_mix[30] < 2 * t.audio_mix[15]);
    return 0;
}

/* --- LFSR sequence hash for AUDC=1 (poly4 noise, period 15) --- */

static int test_poly4_noise_period(void)
{
    struct tia t;
    int16_t buf[128];
    size_t n;
    size_t i;
    tia_init(&t);
    tia_write(&t, 0x15, 0x01);        /* AUDC0 = poly4 */
    tia_write(&t, 0x17, 0x00);
    tia_write(&t, 0x19, 0x08);
    scanlines(&t, 60);                /* 120 samples */
    n = tia_drain_audio(&t, buf, sizeof(buf)/sizeof(buf[0]));
    /* Period is 15 * 2 samples per LFSR step = 30 samples or so. The LFSR
     * output is binary so each sample is 0 or mix[8]. Verify it's not stuck. */
    ASSERT_TRUE(n >= 60);
    {
        int saw_zero = 0, saw_nonzero = 0;
        for (i = 0; i < n; i++) {
            if (buf[i] == 0) saw_zero = 1;
            else if (buf[i] == t.audio_mix[8]) saw_nonzero = 1;
            else {
                fprintf(stderr, "unexpected sample %zu = %d\n", i, buf[i]);
                return 1;
            }
        }
        ASSERT_TRUE(saw_zero && saw_nonzero);
    }
    return 0;
}

/* --- Both channels mix with compression --- */

static int test_two_channels_sum_through_mix(void)
{
    struct tia t;
    int16_t buf[32];
    size_t n;
    size_t i;
    tia_init(&t);
    /* Both channels in AUDC=0 DC mode at AUDV=15 → sum=30 → max. */
    tia_write(&t, 0x15, 0x00); tia_write(&t, 0x16, 0x00);
    tia_write(&t, 0x19, 0x0F); tia_write(&t, 0x1A, 0x0F);
    scanlines(&t, 10);
    n = tia_drain_audio(&t, buf, sizeof(buf)/sizeof(buf[0]));
    ASSERT_TRUE(n >= 10);
    /* After settling: both channels DC at AUDV=15, sum=30, mix[30]=0x7FFF. */
    for (i = 8; i < n; i++) {
        if (buf[i] != 32767) {
            fprintf(stderr, "sample %zu: expected 32767 got %d\n", i, buf[i]);
            return 1;
        }
    }
    return 0;
}

/* --- Serialize round-trip keeps audio state --- */

static int test_serialize_audio(void)
{
    struct tia a, b;
    uint8_t buf[128];
    int16_t s_a[32], s_b[32];
    size_t na, nb;
    size_t sz = tia_serialize_size();
    ASSERT_TRUE(sz <= sizeof(buf));
    tia_init(&a);
    tia_write(&a, 0x15, 0x04);        /* AUDC0 pure tone */
    tia_write(&a, 0x17, 0x02);
    tia_write(&a, 0x19, 0x0A);
    tick_n(&a, 500);                   /* run for a bit */
    tia_drain_audio(&a, s_a, sizeof(s_a)/sizeof(s_a[0]));

    tia_serialize(&a, buf);
    tia_init(&b);
    ASSERT_TRUE(tia_deserialize(&b, buf, sz));

    /* Both should produce the same next samples. */
    scanlines(&a, 5);
    scanlines(&b, 5);
    na = tia_drain_audio(&a, s_a, sizeof(s_a)/sizeof(s_a[0]));
    nb = tia_drain_audio(&b, s_b, sizeof(s_b)/sizeof(s_b[0]));
    ASSERT_EQ(na, nb);
    {
        size_t i;
        for (i = 0; i < na; i++) ASSERT_EQ(s_a[i], s_b[i]);
    }
    return 0;
}

TEST_MAIN_BEGIN
    RUN_TEST(test_audc_masks_to_4_bits);
    RUN_TEST(test_audf_masks_to_5_bits);
    RUN_TEST(test_audv_masks_to_4_bits);
    RUN_TEST(test_two_samples_per_scanline);
    RUN_TEST(test_silence_with_audv_zero);
    RUN_TEST(test_audc_zero_produces_dc);
    RUN_TEST(test_audc_pure_tone_alternates);
    RUN_TEST(test_drain_partial);
    RUN_TEST(test_audf_change_preserves_divider);
    RUN_TEST(test_mix_table_compression);
    RUN_TEST(test_poly4_noise_period);
    RUN_TEST(test_two_channels_sum_through_mix);
    RUN_TEST(test_serialize_audio);
TEST_MAIN_END
