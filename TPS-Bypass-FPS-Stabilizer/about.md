# TPS Bypass & FPS Stabilizer

## What does it do?

Two independent features, both configurable in mod settings:

1. **FPS Stabilizer (dt clamp)** — the actual fix for stutter/frame drops
   when running at high FPS caps. Clamps abnormally large frame times
   before they reach the game update loop, so a stutter/hitch doesn't
   translate into a visible jump in physics or animation. Safe to leave
   enabled.

2. **TPS Bypass (physics substepping)** — EXPERIMENTAL. Runs the game's
   update loop multiple times per rendered frame (each with a
   proportionally smaller dt) instead of once. This does not raise your
   actual render FPS; it raises how many times game logic ticks per
   rendered frame. Because it re-runs the *entire* update loop (not an
   isolated physics-only function), it is very likely to break vanilla
   physics parity, replay/bot accuracy, and may interact badly with other
   gameplay mods. Default multiplier is 1 (off / vanilla).

## Why this is separate from Superb Input Precision / Subtick Inputs API

Those mods are about *input timing precision* (when a click registers
between physics steps), not about the physics tick rate itself or render
frame pacing. This mod is unrelated to their codebase — it hooks
`GJBaseGameLayer::update` directly instead of the input queue.

## Warnings

- This mod is likely to be flagged as gameplay-altering; do not use on
  levels/records you intend to submit as legitimate.
- Physics substepping (>1) is experimental and not vanilla-accurate.
  Test in practice mode first.
- If you experience crashes, try setting the multiplier back to 1 before
  reporting.

## Credits

Structure and hook-priority pattern borrowed from the conventions used in
Superb Input Precision / Subtick Inputs API by chizz.
