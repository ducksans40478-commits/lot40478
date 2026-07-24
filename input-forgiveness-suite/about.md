# Better input

Addon for Subtick Inputs API. Bundles several input-forgiveness features
into a single mod so they share state instead of competing over the same
hooks. This originally started as a few separate mods, but keeping them
in sync quickly became more work than the features themselves.

Only uses `useVanilla()` and `Config` from SIAPI's public API. No
private headers, binary patching, or offset hooks, so it should behave
the same across Windows, macOS, Android, and iOS.

## Features

**Coyote Time** — allows jumps for a short time after leaving the ground.
Sometimes your timing is only a frame late.

**Jump Buffer** — stores an early jump input and executes it on the first
possible frame after landing.

**Re-grant Debounce** — enforces a minimum delay between forgiveness
grants so Coyote Time and Jump Buffer can't accidentally chain into a
double jump. Spam clicking won't change its mind.

**Mode Transition Reset** — clears any pending coyote or buffered jump
state whenever a portal changes your gamemode, preventing old state from
leaking into a completely different movement mode.

**Robot Boost Coyote** *(off by default)* — extends the coyote window
while Robot is boosted from a pad. Gravity is briefly suppressed during
the boost, making the normal timing window feel a little too strict.

**Jump Boost** *(off by default)* — adds a small amount of extra jump
velocity. This isn't intended to "fix" SIAPI—the slightly lower jump
height from late-tick inputs is already correct physics. Enable it if
you simply prefer a stronger jump.

**Straight Fly / Smooth Swing** *(off by default)* — smooths Ship and
Swing Y velocity between ticks to reduce visible jitter while holding.
Smoothing is immediately discarded after gravity flips and gamemode
changes, since those are real motion changes rather than visual noise.

**Ship Jolt Guard** *(experimental, off by default)* — limits unusually
large one-frame velocity spikes that can occasionally happen during Ship
gravity flips. It doesn't replace or override SIAPI's gravity
calculation; it only clamps the resulting velocity change. If it feels
worse than the problem, just leave it disabled.

**Wave Consistency HUD** *(off by default)* — lightweight read-only
overlay showing how consistent your Wave clicks are compared to a rolling
average, first click, or manual target. It never modifies input,
physics, or queued buttons—it only watches what's already happening.

Coyote Time, Jump Buffer, Re-grant Debounce, and Robot Boost Coyote
apply to Cube, Ball, Robot, and Spider—anything with a discrete jump
press. Ship, Swing, Wave, Dash, and orb interactions are intentionally
ignored. Orbs already buffer themselves, and hold-based gamemodes don't
really have a jump press to buffer. Ship and Swing use Straight Fly /
Smooth Swing instead.

### Known limitation

Coyote and buffered jumps are dispatched through `handleButton()`
directly, so they bypass SIAPI's subtick displacement correction. That
correction only applies to entries already stored in
`PlayLayer::m_queuedButtons`, and SIAPI doesn't currently expose the
queue's element type.

The result behaves like a real click at `r = 0` rather than a blended
subtick click. Nothing is actually broken—it just isn't quite as
precise as a native subtick input would be. Properly fixing this would
require SIAPI to expose an API for injecting inputs at an arbitrary
subtick ratio.

### Not included

Corner-clip forgiveness isn't included. That problem depends on
collision geometry and hitboxes rather than input timing, making it a
completely different feature. Better kept separate than bolted onto this
one.

## Requirements

- Subtick Inputs API `>= v0.3.1` (required).
- Superb Input Precision is optional, but the smoothing features and
  Wave Consistency HUD automatically respect Velocity Unrounding if it's
  available.

Coyote Time, Jump Buffer, and Re-grant Debounce are enabled by default
(50 ms / 50 ms / 40 ms). Everything else is opt-in, and every value can
be adjusted from the in-game settings panel.

## Compatibility

- Incompatible with Click Between Frames, CBF Lite, and Click After
  Frames—for the same reason Subtick Inputs API itself is incompatible
  with them.
- Incompatible with `user.coyote-time` and `user.jump-height-fix` since
  they modify the same gameplay state (`m_isOnGround` and jump
  velocity). Running both usually ends with two mods trying to solve the
  same problem.
- Fully compatible with Superb Input Precision.

## Disclaimer

Ship Jolt Guard is still experimental. It clamps the symptom rather than
the underlying cause because SIAPI doesn't currently expose its internal
gravity coefficient. If it ends up clipping a legitimate fast gravity
flip, lower the threshold or simply turn it off.

Everything else has been stable throughout testing, but as always, test
it yourself before relying on it. Better to find surprises in practice
than in the middle of a good run.