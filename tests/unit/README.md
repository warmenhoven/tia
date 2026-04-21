# TIA unit tests

Two kinds of tests live in this directory:

## Regression guards (most of what's here today)
Tests that lock current behavior. They don't prove correctness — they catch
regressions. Valuable because they fail loudly if a refactor silently changes
the formula. When you see `ASSERT_EQ(t.p0_pos, 100 + 5 - TIA_HBLANK_CLOCKS)`
that's a regression guard: if the internal offset changes, the test fires.

## Doc-anchored behavioral tests (the ones we're adding)
Tests whose expected values come from documentation or an independent
reference, cited in the test's doc comment. They prove the implementation
matches the thing we're trying to emulate, not just itself.

### Sources of truth (in order of preference)

- **SPG** — Steve Wright's *Programmer's Guide* (Atari, 1979). The
  original register-level doc for the TIA. Citations use "SPG §section".
- **Towers** — *TIA: A Hardware Analysis* by Andrew Towers. Die-shot-derived
  timing, especially RES*x pipeline.
- **Brenner** — *Atari 2600 TIA Audio* by Chris Brenner. Die-shot-derived
  audio channel model.

### Doc comment format

```c
/* Documented: SPG §GRP0 — "bit 7 is the leftmost pixel, bit 0 rightmost".
 * Test: place single-bit sprites and verify column of visible pixel. */
static int test_grp0_bit_order(void) { ... }
```

For things observed in hardware only:

```c
/* Hardware behavior: VBLANK bit 7 does NOT ground INPT4/5
 * — that bit is the paddle DUMP line only. */
```

When something is purely a regression guard, mark it:

```c
/* Regression guard: matches our behavior as of <commit> — not doc-anchored. */
```

## Adding new tests

Prefer behavioral assertions (read `t->fb` after rendering) over state
assertions (`t->p0_pos`). The former verifies emulation; the latter verifies
internal consistency with itself.
