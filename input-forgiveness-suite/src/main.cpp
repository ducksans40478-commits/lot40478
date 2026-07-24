// addon for chizz.subtick-inputs-api
// coyote time, jump buffer, hold-flight smoothing (ship+swing), robot boost coyote widen,
// jump boost, wave consistency hud (read only, bottom of file)
//
// stuff we DON'T touch: ship/wave/swing/dash (hold modes, coyote/buffer don't make sense there),
// orb touches (they already buffer natively), locked/hidden input state
//
// no binary offset patching, should be fine cross platform
//
// known issue: coyote/buffered jumps we fire via handleButton() don't get SIAPI's subtick
// displacement correction, because that only applies to entries already in
// PlayLayer::m_queuedButtons and the element type for that queue isn't exposed in
// SubtickInputs.hpp. so these jumps behave like a real click at ratio r=0 - not broken,
// just not blended. would need SIAPI to expose some kind of "inject at ratio r" call.
//
// gravity flip note: getGravityCoefficient() in SIAPI branches on playerIsFallingBugged()
// (upside down state) - that's not our problem to recompute. what IS ours: the hold-flight
// smoothing blends m_yVelocity across ticks, and a flip is a real discontinuity not jitter,
// so we re-snap on flip instead of smoothing through it. same idea as the mode reset below.

#include <cmath>
#include <algorithm>
#include <deque>
#include <chrono>

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <chizz.subtick-inputs-api/include/SubtickInputs.hpp>

using namespace geode::prelude;
using namespace subtickinputs;

// wave hud stuff is at the bottom, needs a fwd decl up here since handleButton() calls it
void waveHudLoadSettings();
void waveHudMaybeRecordClick(bool isPush, int button, bool isPlayer1);

// settings mirrors --------------------------------------------------------

static bool   s_coyoteEnabled  = true;
static double s_coyoteWindowMs = 50.0;

static bool   s_bufferEnabled  = true;
static double s_bufferWindowMs = 50.0;

static bool   s_debounceEnabled = true;
static double s_debounceMs      = 40.0;

// setting keys are still "straight-fly" for save compat, ship+swing use the same
// smoothing curve now so didn't bother renaming
static bool   s_straightFlyEnabled   = false; // ship
static bool   s_smoothSwingEnabled   = false; // swing
static double s_straightFlySmoothing = 0.2;
static double s_straightFlyGraceMs   = 80.0;

static bool   s_robotBoostCoyoteEnabled    = false;
static double s_robotBoostCoyoteMultiplier = 1.5; // x s_coyoteWindowMs

static bool   s_jumpBoostEnabled = false;
static double s_jumpBoostAmount  = 0.3; // flat velocity add on a real jump

static bool   s_warnSmallWindows = true;
static bool   s_warnGravityFlipAnomaly = false;

static bool   s_shipJoltGuardEnabled  = false; // experimental, off by default, tune by feel
static double s_shipJoltGuardMaxDelta = 3.0;

// per-player state ----------------------------------------------------------

struct ForgivenessState {
    // coyote
    double leftGroundTimestamp = -1.0;
    bool   coyoteConsumed      = false;
    bool   leftGroundWasRobotBoosted = false;

    // buffer
    double bufferedPressTimestamp = -1.0;
    bool   bufferConsumed         = false;
    bool   bufferPressReleased    = false;

    // shared
    bool   wasOnGround        = false;
    bool   hasGroundSample    = false;
    double lastGrantTimestamp = -1000.0;

    // mode transition detection
    uint32_t lastModeSignature = 0;
    bool     hasModeSignature  = false;

    // gravity flip detection
    bool wasUpsideDown       = false;
    bool hasUpsideDownSample = false;

    // jolt guard ref - yVelocity at end of prev frame
    double lastYVelocity     = 0.0;
    bool   hasYVelocitySample = false;

    // hold-flight smoothing (ship/swing)
    bool   jumpHeld                        = false;
    double straightFlySmoothed             = 0.0;
    bool   straightFlyInitialized          = false;
    double straightFlyLastReleaseTimestamp = -1000.0;
};

static ForgivenessState s_p1;
static ForgivenessState s_p2;

static void resetFallState(ForgivenessState& st) {
    st.leftGroundTimestamp       = -1.0;
    st.coyoteConsumed            = false;
    st.leftGroundWasRobotBoosted = false;
    st.bufferedPressTimestamp    = -1.0;
    st.bufferConsumed            = false;
    st.bufferPressReleased       = false;
}

// mode helpers ----------------------------------------------------------

static bool shouldSkipForgiveness(PlayerObject* player) {
    if (!player) return true;
    return player->m_isShip
        || player->m_isDart
        || player->m_isSwing
        || player->m_isDashing
        || player->m_isLocked
        || player->m_isHidden;
}

static bool shouldSkipJumpBoost(PlayerObject* player) {
    if (!player) return true;
    return player->m_isShip
        || player->m_isDart
        || player->m_isSwing
        || player->m_isDashing
        || player->m_isBird; // ufo hops work differently, leave it alone
}

static bool isHoldFlightMode(PlayerObject* player) {
    if (!player) return false;
    return (player->m_isShip && s_straightFlyEnabled)
        || (player->m_isSwing && s_smoothSwingEnabled);
}

// mirrors SIAPI's zero-grav robot boost condition (getGravPerTick in inputs.cpp) -
// gravity is 0 for a tick while boosted+not touched pad+still under accel threshold.
// that fn is file-local in SIAPI so can't call it directly, just copying the condition
static bool isRobotBoostFloating(PlayerObject* player) {
    if (!player) return false;
    return player->m_isRobot
        && player->m_maybeIsBoosted
        && player->m_jumpBuffered
        && !player->m_touchedPad
        && player->m_accelerationOrSpeed < 1.5f;
}

static uint32_t computeModeSignature(PlayerObject* player) {
    if (!player) return 0;
    uint32_t sig = 0;
    sig |= (player->m_isShip   ? 1u << 0 : 0);
    sig |= (player->m_isBall   ? 1u << 1 : 0);
    sig |= (player->m_isBird   ? 1u << 2 : 0);
    sig |= (player->m_isDart   ? 1u << 3 : 0);
    sig |= (player->m_isRobot  ? 1u << 4 : 0);
    sig |= (player->m_isSpider ? 1u << 5 : 0);
    sig |= (player->m_isSwing  ? 1u << 6 : 0);
    return sig;
}

// diagnostic only. mirrors getGravityCoefficient()'s ship condition to guess when a
// flip frame hit the coeff = -1.0 branch (the "ship feels heavy" bug people report).
// no exported access to the real coefficient so this just flags it, doesn't touch anything
static bool shipGravityFlipLikelyAnomalous(PlayerObject* player) {
    if (!player || !player->m_isShip) return false;
    bool upsideDown = player->m_isUpsideDown;
    double yVel = player->m_yVelocity;
    bool wrongDir = (!upsideDown && yVel < 0.0) || (upsideDown && yVel > 0.0);
    if (!wrongDir) return false;
    bool holding = player->m_jumpBuffered;
    bool accel = player->m_isAccelerating;
    return holding ? (!accel || wrongDir) : (accel && wrongDir);
}

static bool debounceOk(ForgivenessState& st, double now) {
    if (!s_debounceEnabled) return true;
    return (now - st.lastGrantTimestamp) * 1000.0 >= s_debounceMs;
}

static bool inCoyoteWindow(ForgivenessState& st, double now) {
    if (!s_coyoteEnabled)             return false;
    if (st.coyoteConsumed)            return false;
    if (st.leftGroundTimestamp < 0.0) return false;

    double windowMs = s_coyoteWindowMs;
    if (s_robotBoostCoyoteEnabled && st.leftGroundWasRobotBoosted) {
        windowMs *= s_robotBoostCoyoteMultiplier;
    }

    double elapsedMs = (now - st.leftGroundTimestamp) * 1000.0;
    return elapsedMs >= 0.0 && elapsedMs <= windowMs;
}

static bool bufferedPressIsValid(ForgivenessState& st, double landedAt) {
    if (!s_bufferEnabled)                return false;
    if (st.bufferConsumed)               return false;
    if (st.bufferedPressTimestamp < 0.0) return false;
    double elapsedMs = (landedAt - st.bufferedPressTimestamp) * 1000.0;
    return elapsedMs >= 0.0 && elapsedMs <= s_bufferWindowMs;
}

// SIAPI checks inputs Config::get().getInputHz() times/sec, so a window smaller than
// one check period can't really be resolved finer than that. just a warning, doesn't
// change behavior
static void warnIfWindowTooSmall(const char* settingName, double windowMs) {
    if (!s_warnSmallWindows) return;

    // instant inputs bypasses the Hz stepping entirely so the floor below doesn't apply
    if (Config::get().isInstantInputsEnabled()) return;

    double hz = Config::get().getInputHz();
    if (hz <= 0.0) return;

    double minResolvableMs = 1000.0 / hz;
    if (windowMs > 0.0 && windowMs < minResolvableMs) {
        log::warn(
            "Input Forgiveness Suite: '{}' is {:.1f}ms, but Subtick Inputs API is "
            "checking inputs every ~{:.1f}ms (Input Hz = {:.0f}). The window will "
            "still work, but can't be resolved any finer than that period.",
            settingName, windowMs, minResolvableMs, hz);
    }
}

// GJBaseGameLayer hook - coyote/buffer/smoothing/boost live here.
// only hooks update() and handleButton(), never processQueuedButtons(), so no race
// with SIP's VeryEarly hook. still setting explicit priority so we're deterministic
// against other mods on the same functions.

class $modify(ForgivenessGJBaseGameLayer, GJBaseGameLayer) {
    static void onModify(auto& self) {
        if (!self.setHookPriorityPost("GJBaseGameLayer::update", Priority::Normal)) {
            log::warn("Input Forgiveness Suite: failed to set hook priority for update().");
        }
        if (!self.setHookPriorityPost("GJBaseGameLayer::handleButton", Priority::Normal)) {
            log::warn("Input Forgiveness Suite: failed to set hook priority for handleButton().");
        }
    }

    void update(float dt) {
        bool anyFeature = s_coyoteEnabled || s_bufferEnabled
            || s_straightFlyEnabled || s_smoothSwingEnabled || s_shipJoltGuardEnabled;

        PlayLayer* pl = PlayLayer::get();

        // grab ground state before the vanilla check below - otherwise a frame where
        // useVanilla() is true (right after unpause etc) could eat a ground/air
        // transition and desync wasOnGround
        if (pl) {
            if (pl->m_player1) s_p1.wasOnGround = pl->m_player1->m_isOnGround;
            if (pl->m_player2 && pl->m_gameState.m_isDualMode)
                s_p2.wasOnGround = pl->m_player2->m_isOnGround;
        }

        if (!anyFeature || useVanilla()) {
            GJBaseGameLayer::update(dt);
            return;
        }

        GJBaseGameLayer::update(dt);

        if (!pl) return;

        double now = pl->m_timestamp;

        auto processPlayer = [&](PlayerObject* player, ForgivenessState& st) {
            if (!player) return;

            // mode reset has to run before the smoothing block below - otherwise a
            // portal flipping ship<->swing mid air could blend the new mode's velocity
            // against straightFlySmoothed from the old mode's completely different scale
            uint32_t sig = computeModeSignature(player);
            if (st.hasModeSignature && sig != st.lastModeSignature) {
                resetFallState(st);

                // ship+swing share the hold-flight fields. resetFallState() runs every
                // frame while in ship/swing (both are in the skip list) so it can't
                // touch straightFly* itself, that'd wipe smoothing constantly. only
                // reset those when the transition actually crosses ship<->swing
                constexpr uint32_t kShipBit        = 1u << 0;
                constexpr uint32_t kSwingBit       = 1u << 6;
                constexpr uint32_t kHoldFlightMask = kShipBit | kSwingBit;
                if ((sig & kHoldFlightMask) != (st.lastModeSignature & kHoldFlightMask)) {
                    st.straightFlyInitialized = false;
                    st.straightFlySmoothed    = 0.0;
                    // leaving jumpHeld alone on purpose, button state didn't change
                    // just because a portal swapped the gamemode under it
                }
            }
            st.lastModeSignature = sig;
            st.hasModeSignature  = true;

            // flip mid flight is a real instant change in target velocity, not jitter,
            // so re-snap instead of smoothing through it
            bool upsideDown = player->m_isUpsideDown;
            if (st.hasUpsideDownSample && upsideDown != st.wasUpsideDown) {
                st.straightFlyInitialized = false;

                bool anomalous = shipGravityFlipLikelyAnomalous(player);

                if (s_warnGravityFlipAnomaly && anomalous) {
                    log::warn(
                        "Input Forgiveness Suite: ship gravity flip at yVel={:.2f}, "
                        "upsideDown={} — likely hit SIAPI's anomalous coeff=-1.0 branch "
                        "for this tick (diagnostic only, no physics changed).",
                        player->m_yVelocity, upsideDown);
                }

                // jolt guard - experimental/opt-in/off by default. doesn't recompute
                // the gravity coeff (no way to get at it), just clamps the symptom -
                // an oversized one tick velocity jump - to a hand tuned max when we
                // think we hit the anomalous branch. heuristic, not a real fix
                if (s_shipJoltGuardEnabled && anomalous && st.hasYVelocitySample) {
                    double preVel  = st.lastYVelocity;
                    double postVel = player->m_yVelocity;
                    double delta   = postVel - preVel;
                    double maxDelta = s_shipJoltGuardMaxDelta;
                    if (std::abs(delta) > maxDelta) {
                        double clampedDelta = std::copysign(maxDelta, delta);
                        player->setYVelocity(preVel + clampedDelta, 0);
                    }
                }
            }
            st.wasUpsideDown       = upsideDown;
            st.hasUpsideDownSample = true;

            // hold-flight smoothing (ship/swing)
            if (isHoldFlightMode(player) && st.jumpHeld) {
                double target = player->m_yVelocity;
                double sinceLastRelease = (now - st.straightFlyLastReleaseTimestamp) * 1000.0;
                bool withinGrace = st.straightFlyInitialized
                    && sinceLastRelease <= s_straightFlyGraceMs;

                if (!withinGrace) {
                    st.straightFlySmoothed    = target;
                    st.straightFlyInitialized = true;
                } else {
                    double factor = s_straightFlySmoothing;
                    st.straightFlySmoothed = st.straightFlySmoothed * factor + target * (1.0 - factor);
                }

                // going through the setter so it dispatches to SIAPI's SIPlayerObject
                // override and respects Velocity Unrounding instead of fighting it.
                // type=0 same as SIAPI's own setYVelocity(0,0) call for dashing
                player->setYVelocity(st.straightFlySmoothed, 0);
            } else if (!st.jumpHeld) {
                double sinceLastRelease = (now - st.straightFlyLastReleaseTimestamp) * 1000.0;
                if (sinceLastRelease > s_straightFlyGraceMs)
                    st.straightFlyInitialized = false;
            }

            if (shouldSkipForgiveness(player)) {
                resetFallState(st);
                return;
            }

            bool onGround = player->m_isOnGround;
            bool hadSample = st.hasGroundSample;
            st.hasGroundSample = true;

            if (hadSample && st.wasOnGround && !onGround) {
                // just left ground, open the coyote window
                st.leftGroundTimestamp       = now;
                st.coyoteConsumed            = false;
                st.leftGroundWasRobotBoosted = isRobotBoostFloating(player);

            } else if (hadSample && !st.wasOnGround && onGround) {
                // just landed, try firing a buffered press
                if (bufferedPressIsValid(st, now) && debounceOk(st, now)) {
                    bool isP1 = (player == pl->m_player1);
                    pl->handleButton(true, 1, isP1);
                    if (st.bufferPressReleased)
                        pl->handleButton(false, 1, isP1);
                    st.bufferConsumed     = true;
                    st.lastGrantTimestamp = now;
                }
                st.leftGroundTimestamp       = -1.0;
                st.coyoteConsumed            = false;
                st.leftGroundWasRobotBoosted = false;
                st.bufferedPressTimestamp    = -1.0;
                st.bufferPressReleased       = false;
            }
        };

        processPlayer(pl->m_player1, s_p1);
        if (pl->m_gameState.m_isDualMode)
            processPlayer(pl->m_player2, s_p2);
    }

    void handleButton(bool isPush, int button, bool isPlayer1) {
        bool anyFeature = s_coyoteEnabled || s_bufferEnabled
            || s_straightFlyEnabled || s_smoothSwingEnabled || s_jumpBoostEnabled;

        // has to record every real wave click regardless of whether anything below is
        // active - this never changes what handleButton actually dispatches to the engine
        waveHudMaybeRecordClick(isPush, button, isPlayer1);

        if (!anyFeature || button != 1 || useVanilla()) {
            GJBaseGameLayer::handleButton(isPush, button, isPlayer1);
            return;
        }

        PlayLayer* pl = PlayLayer::get();
        if (!pl) {
            GJBaseGameLayer::handleButton(isPush, button, isPlayer1);
            return;
        }

        PlayerObject*     player = isPlayer1 ? pl->m_player1 : pl->m_player2;
        ForgivenessState& st     = isPlayer1 ? s_p1 : s_p2;

        if (!isPush) {
            if (st.bufferedPressTimestamp >= 0.0 && !st.bufferConsumed)
                st.bufferPressReleased = true;
            st.jumpHeld = false;
            st.straightFlyLastReleaseTimestamp = pl->m_timestamp;
            GJBaseGameLayer::handleButton(isPush, button, isPlayer1);
            return;
        }

        st.jumpHeld = true;

        bool grantedCoyote = false;

        bool touchingOrb = player
            && player->m_touchingRings
            && player->m_touchingRings->count() > 0;

        if (player && !shouldSkipForgiveness(player) && !touchingOrb) {
            if (!player->m_isOnGround) {
                if (inCoyoteWindow(st, pl->m_timestamp) && debounceOk(st, pl->m_timestamp)) {
                    player->m_isOnGround  = true;
                    grantedCoyote          = true;
                    st.coyoteConsumed      = true;
                    st.lastGrantTimestamp  = pl->m_timestamp;
                } else {
                    st.bufferedPressTimestamp = pl->m_timestamp;
                    st.bufferConsumed         = false;
                    st.bufferPressReleased    = false;
                }
            }
        }

        bool applyBoost = s_jumpBoostEnabled
            && player
            && !touchingOrb
            && !shouldSkipJumpBoost(player);
        double preJumpVel = applyBoost ? player->m_yVelocity : 0.0;

        GJBaseGameLayer::handleButton(isPush, button, isPlayer1);

        if (grantedCoyote && player)
            player->m_isOnGround = false;

        if (applyBoost) {
            double deltaVel = player->m_yVelocity - preJumpVel;
            if (std::abs(deltaVel) > 0.001) {
                double sign = deltaVel > 0.0 ? 1.0 : -1.0;
                double boosted = player->m_yVelocity + sign * s_jumpBoostAmount;
                // same reasoning as the smoothing setter above, go through the setter
                player->setYVelocity(boosted, 0);
            }
        }

        if (player && !grantedCoyote
            && st.bufferedPressTimestamp >= 0.0
            && !st.bufferConsumed
            && player->m_isOnGround) {
            st.bufferedPressTimestamp = -1.0;
            st.bufferPressReleased    = false;
        }
    }
};

// lifecycle ---------------------------------------------------------------

$on_mod(Loaded) {
    auto* mod = Mod::get();

    s_coyoteEnabled  = mod->getSettingValue<bool>  ("coyote-enabled");
    s_coyoteWindowMs = mod->getSettingValue<double>("coyote-window-ms");
    s_bufferEnabled  = mod->getSettingValue<bool>  ("buffer-enabled");
    s_bufferWindowMs = mod->getSettingValue<double>("buffer-window-ms");
    s_debounceEnabled = mod->getSettingValue<bool>  ("debounce-enabled");
    s_debounceMs      = mod->getSettingValue<double>("debounce-ms");
    s_straightFlyEnabled   = mod->getSettingValue<bool>  ("straight-fly-enabled");
    s_smoothSwingEnabled   = mod->getSettingValue<bool>  ("smooth-swing-enabled");
    s_straightFlySmoothing = mod->getSettingValue<double>("straight-fly-smoothing");
    s_straightFlyGraceMs   = mod->getSettingValue<double>("straight-fly-grace-ms");
    s_robotBoostCoyoteEnabled    = mod->getSettingValue<bool>  ("robot-boost-coyote-enabled");
    s_robotBoostCoyoteMultiplier = mod->getSettingValue<double>("robot-boost-coyote-multiplier");
    s_jumpBoostEnabled     = mod->getSettingValue<bool>  ("jump-boost-enabled");
    s_jumpBoostAmount      = mod->getSettingValue<double>("jump-boost-amount");
    s_warnSmallWindows     = mod->getSettingValue<bool>  ("warn-small-windows");
    s_warnGravityFlipAnomaly = mod->getSettingValue<bool> ("warn-gravity-flip-anomaly");

    warnIfWindowTooSmall("coyote-window-ms", s_coyoteWindowMs);
    warnIfWindowTooSmall("buffer-window-ms", s_bufferWindowMs);

    listenForSettingChanges<bool>  ("coyote-enabled",   +[](bool   v) { s_coyoteEnabled = v; });
    listenForSettingChanges<double>("coyote-window-ms", +[](double v) {
        s_coyoteWindowMs = v;
        warnIfWindowTooSmall("coyote-window-ms", v);
    });
    listenForSettingChanges<bool>  ("buffer-enabled",   +[](bool   v) { s_bufferEnabled = v; });
    listenForSettingChanges<double>("buffer-window-ms", +[](double v) {
        s_bufferWindowMs = v;
        warnIfWindowTooSmall("buffer-window-ms", v);
    });
    listenForSettingChanges<bool>  ("debounce-enabled",       +[](bool   v) { s_debounceEnabled      = v; });
    listenForSettingChanges<double>("debounce-ms",            +[](double v) { s_debounceMs           = v; });
    listenForSettingChanges<bool>  ("straight-fly-enabled",   +[](bool   v) { s_straightFlyEnabled   = v; });
    listenForSettingChanges<bool>  ("smooth-swing-enabled",   +[](bool   v) { s_smoothSwingEnabled   = v; });
    listenForSettingChanges<double>("straight-fly-smoothing", +[](double v) { s_straightFlySmoothing = v; });
    listenForSettingChanges<double>("straight-fly-grace-ms",  +[](double v) { s_straightFlyGraceMs   = v; });
    listenForSettingChanges<bool>  ("robot-boost-coyote-enabled",    +[](bool   v) { s_robotBoostCoyoteEnabled    = v; });
    listenForSettingChanges<double>("robot-boost-coyote-multiplier", +[](double v) { s_robotBoostCoyoteMultiplier = v; });
    listenForSettingChanges<bool>  ("jump-boost-enabled",     +[](bool   v) { s_jumpBoostEnabled     = v; });
    listenForSettingChanges<double>("jump-boost-amount",      +[](double v) { s_jumpBoostAmount      = v; });
    listenForSettingChanges<bool>  ("warn-small-windows",     +[](bool   v) { s_warnSmallWindows     = v; });
    listenForSettingChanges<bool>  ("warn-gravity-flip-anomaly", +[](bool v) { s_warnGravityFlipAnomaly = v; });

    waveHudLoadSettings();
}


// wave consistency hud - separate feature, read only, never touches m_yVelocity,
// position, input queue or m_isOnGround. just watches the button-push events already
// flowing through handleButton above to timestamp wave clicks and draw an overlay

namespace {

    struct ClickSample {
        double timestampMs;
        double deltaFromPrevMs; // 0 on the very first sample
    };

    class TimingTracker {
    public:
        static TimingTracker& get() {
            static TimingTracker instance;
            return instance;
        }

        void reset() {
            m_history.clear();
            m_lastClickMs = -1.0;
        }

        void recordClick(double nowMs, int historySize) {
            double delta = 0.0;
            if (m_lastClickMs >= 0.0) {
                delta = nowMs - m_lastClickMs;
            }
            m_lastClickMs = nowMs;

            m_history.push_back({nowMs, delta});
            while (static_cast<int>(m_history.size()) > historySize) {
                m_history.pop_front();
            }
        }

        bool latestDeviation(double& outDeviationMs, double& outIntervalMs,
                              const std::string& mode, double manualTargetMs) {
            if (m_history.size() < 2) return false;

            double target = computeTargetIntervalMs(mode, manualTargetMs);
            if (target <= 0.0) return false;

            double lastInterval = m_history.back().deltaFromPrevMs;
            outIntervalMs = lastInterval;
            outDeviationMs = lastInterval - target;
            return true;
        }

        double computeTargetIntervalMs(const std::string& mode, double manualTargetMs) {
            if (mode == "manual") {
                return manualTargetMs;
            }

            if (mode == "first-click") {
                for (auto& s : m_history) {
                    if (s.deltaFromPrevMs > 0.0) return s.deltaFromPrevMs;
                }
                return 0.0;
            }

            // rolling-average (default)
            double sum = 0.0;
            int count = 0;
            for (size_t i = 0; i + 1 < m_history.size(); ++i) {
                if (m_history[i].deltaFromPrevMs > 0.0) {
                    sum += m_history[i].deltaFromPrevMs;
                    count++;
                }
            }
            if (count == 0) return 0.0;
            return sum / count;
        }

        const std::deque<ClickSample>& history() const { return m_history; }

    private:
        std::deque<ClickSample> m_history;
        double m_lastClickMs = -1.0;
    };

    double nowMsWallClock() {
        using namespace std::chrono;
        return duration<double, std::milli>(
            high_resolution_clock::now().time_since_epoch()
        ).count();
    }

    // kept separate from the settings mirrors up top so this whole section can be
    // ripped out on its own later if needed
    bool        s_waveHudEnabled       = false;
    std::string s_waveHudTargetMode    = "rolling-average";
    double      s_waveHudManualTarget  = 100.0;
    int         s_waveHudHistorySize   = 20;
    double      s_waveHudGoodMs        = 8.0;
    double      s_waveHudBadMs         = 20.0;
    double      s_waveHudScale         = 1.0;

}

void waveHudLoadSettings() {
    auto* mod = Mod::get();
    s_waveHudEnabled      = mod->getSettingValue<bool>       ("wave-hud-enabled");
    s_waveHudTargetMode   = mod->getSettingValue<std::string>("wave-hud-target-mode");
    s_waveHudManualTarget = mod->getSettingValue<double>      ("wave-hud-manual-target-ms");
    s_waveHudHistorySize  = static_cast<int>(mod->getSettingValue<int64_t>("wave-hud-history-size"));
    s_waveHudGoodMs       = mod->getSettingValue<double>      ("wave-hud-good-threshold-ms");
    s_waveHudBadMs        = mod->getSettingValue<double>      ("wave-hud-bad-threshold-ms");
    s_waveHudScale        = mod->getSettingValue<double>      ("wave-hud-scale");

    listenForSettingChanges<bool>       ("wave-hud-enabled", +[](bool v) { s_waveHudEnabled = v; });
    listenForSettingChanges<std::string>("wave-hud-target-mode", +[](std::string v) { s_waveHudTargetMode = v; });
    listenForSettingChanges<double>     ("wave-hud-manual-target-ms", +[](double v) { s_waveHudManualTarget = v; });
    listenForSettingChanges<int64_t>    ("wave-hud-history-size", +[](int64_t v) { s_waveHudHistorySize = static_cast<int>(v); });
    listenForSettingChanges<double>     ("wave-hud-good-threshold-ms", +[](double v) { s_waveHudGoodMs = v; });
    listenForSettingChanges<double>     ("wave-hud-bad-threshold-ms", +[](double v) { s_waveHudBadMs = v; });
    listenForSettingChanges<double>     ("wave-hud-scale", +[](double v) { s_waveHudScale = v; });
}

// pure observer, called from handleButton() above before the dispatch logic. only
// records a timestamp, never touches player state
void waveHudMaybeRecordClick(bool isPush, int button, bool isPlayer1) {
    if (!s_waveHudEnabled) return;
    if (!isPush) return;          // press only, not release
    if (!isPlayer1) return;       // p1 only
    if (button != 1) return;      // jump button only

    PlayLayer* pl = PlayLayer::get();
    if (!pl || !pl->m_player1) return;

    // only track while "click" == direction flip, ie wave
    if (!pl->m_player1->m_isDart) return;

    TimingTracker::get().recordClick(nowMsWallClock(), s_waveHudHistorySize);
}

class WaveConsistencyHUD : public CCNode {
public:
    static WaveConsistencyHUD* create() {
        auto ret = new WaveConsistencyHUD();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    bool init() override {
        if (!CCNode::init()) return false;

        this->setScale(static_cast<float>(s_waveHudScale));

        m_label = CCLabelBMFont::create("-- ms", "bigFont.fnt");
        m_label->setAnchorPoint({0.5f, 0.5f});
        m_label->setScale(0.4f);
        this->addChild(m_label);

        m_historyNode = CCDrawNode::create();
        m_historyNode->setPosition({0, -20});
        this->addChild(m_historyNode);

        this->scheduleUpdate();
        return true;
    }

    void update(float dt) override {
        if (!s_waveHudEnabled) {
            this->setVisible(false);
            return;
        }
        this->setVisible(true);

        auto& tracker = TimingTracker::get();

        double deviation, interval;
        if (tracker.latestDeviation(deviation, interval, s_waveHudTargetMode, s_waveHudManualTarget)) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%+.1f ms  (%.1f ms)", deviation, interval);
            m_label->setString(buf);

            double absDev = std::fabs(deviation);
            ccColor3B color;
            if (absDev <= s_waveHudGoodMs) color = {0, 255, 0};
            else if (absDev <= s_waveHudBadMs) color = {255, 255, 0};
            else color = {255, 60, 60};

            m_label->setColor(color);
        } else {
            m_label->setString("-- ms");
            m_label->setColor({255, 255, 255});
        }

        drawHistoryStrip();
    }

private:
    void drawHistoryStrip() {
        m_historyNode->clear();

        auto& tracker = TimingTracker::get();
        auto& hist = tracker.history();
        if (hist.size() < 2) return;

        double target = tracker.computeTargetIntervalMs(s_waveHudTargetMode, s_waveHudManualTarget);
        if (target <= 0.0) return;

        float barWidth = 4.f;
        float spacing = 2.f;
        float x = 0.f;

        for (auto& sample : hist) {
            if (sample.deltaFromPrevMs <= 0.0) continue;

            double dev = std::fabs(sample.deltaFromPrevMs - target);
            ccColor4F color;
            if (dev <= s_waveHudGoodMs) color = {0.f, 1.f, 0.f, 1.f};
            else if (dev <= s_waveHudBadMs) color = {1.f, 1.f, 0.f, 1.f};
            else color = {1.f, 0.2f, 0.2f, 1.f};

            float height = std::clamp(static_cast<float>(dev), 1.f, 30.f);

            m_historyNode->drawRect(
                {x, 0.f}, {x + barWidth, height},
                color, 0.f, color
            );

            x += barWidth + spacing;
        }
    }

    CCLabelBMFont* m_label = nullptr;
    CCDrawNode* m_historyNode = nullptr;
};

class $modify(WaveHudPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        TimingTracker::get().reset();

        auto winSize = CCDirector::sharedDirector()->getWinSize();

        auto hud = WaveConsistencyHUD::create();
        hud->setID("wave-consistency-hud"_spr);
        hud->setPosition({100.f, winSize.height - 60.f});
        hud->setZOrder(1000);
        this->addChild(hud);

        return true;
    }
};