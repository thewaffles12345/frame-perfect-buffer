// Frame Perfect Buffer
//
// Holds back a jump press that arrived too early and forwards it on the tick
// where it is actually useful.  "Tick" means one physics step: GD 2.2 runs the
// physics at a fixed 240 steps per second regardless of render FPS, which is the
// "240 fps frame" people mean when they talk about frame-perfect inputs.
//
// Guiding rule: a press that would do something useful *right now* is never
// delayed.  Only presses that would currently be wasted or harmful are held:
//
//   * ORB AHEAD (default, every mode)
//       The player is not touching an orb, but their trajectory meets one within
//       the next few ticks.  Pressing now would be wasted (cube mid-air), or
//       worse, change the trajectory (UFO flap, ship/swing lift) so the orb is
//       missed or hit at the wrong spot.  The press is held and fired on the
//       first tick the game registers contact with the orb, which is exactly
//       the tick a frame-perfect orb click lands on.
//
//   * GROUND EDGE (opt-in, experimental)
//       The player is grounded; the press is held until the last tick with
//       solid ground under them.  This is a guess at the designer's intent and
//       delays every ordinary grounded jump inside the window, so it is off by
//       default.
//
// A held press always fires when the player is about to overlap a hazard, and
// always fires when the max-delay cap runs out.  If a press is held past the
// tick it arrived on, the attempt is marked and (with Safe Mode on) its progress
// and completion are not saved.

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJGameLevel.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace geode::prelude;

namespace {

// ---------------------------------------------------------------------------
// Settings (cached; the per-tick path never touches the settings system)
// ---------------------------------------------------------------------------

struct Config {
    bool enabled = true;
    int maxDelay = 8;              // ticks
    float hazardScale = 0.3f;      // inner hitbox size relative to full hitbox
    bool groundEdge = false;       // opt-in ground-edge buffering
    bool player2 = true;
    bool safeMode = true;
    bool showIndicator = true;
};

Config g_cfg;

void reloadConfig() {
    auto* mod = Mod::get();
    g_cfg.enabled = mod->getSettingValue<bool>("enabled");
    g_cfg.maxDelay = static_cast<int>(mod->getSettingValue<int64_t>("max-delay"));
    g_cfg.hazardScale = static_cast<float>(mod->getSettingValue<double>("hazard-hitbox-scale"));
    g_cfg.groundEdge = mod->getSettingValue<bool>("ground-edge");
    g_cfg.player2 = mod->getSettingValue<bool>("player-2");
    g_cfg.safeMode = mod->getSettingValue<bool>("safe-mode");
    g_cfg.showIndicator = mod->getSettingValue<bool>("show-indicator");
}

// ---------------------------------------------------------------------------
// Per-player buffer state
// ---------------------------------------------------------------------------

enum class HoldKind { None, Orb, GroundEdge };
enum class FireReason { None, Orb, Edge, Hazard, Cap, Left, Immediate };

char const* reasonName(FireReason r) {
    switch (r) {
        case FireReason::Orb: return "orb";
        case FireReason::Edge: return "edge";
        case FireReason::Hazard: return "hazard";
        case FireReason::Cap: return "cap";
        case FireReason::Left: return "left";
        case FireReason::Immediate: return "now";
        default: return "-";
    }
}

struct Slot {
    bool pending = false;          // a press has been received and not yet forwarded
    HoldKind kind = HoldKind::None;
    bool viaPlayer1Input = true;   // the isPlayer1 flag the press arrived with
    int ticksHeld = 0;             // physics ticks since the press arrived
    bool releaseQueued = false;    // the matching release arrived while pending
    int releaseDelay = 0;          // ticks between the press and that release
    int releaseCountdown = 0;      // >0: forward a release after this many ticks

    // Motion samples, updated once per physics tick.
    CCPoint lastPos{};
    CCPoint lastDelta{};           // per-tick velocity
    CCPoint lastAccel{};           // per-tick change in velocity
    int samples = 0;               // 0: nothing, 1: have lastPos, 2: have lastDelta, 3+: have lastAccel

    void clear() { *this = Slot{}; }
};

Slot g_slots[2];                   // 0 = player 1, 1 = player 2
bool g_passthrough = false;        // true while we call the real handleButton ourselves

// Per-attempt bookkeeping
int g_correctedCount = 0;
int g_correctedTicks = 0;
bool g_attemptTainted = false;     // some press in this attempt was actually delayed
FireReason g_lastReason = FireReason::None;
int g_lastDelay = 0;

// ---------------------------------------------------------------------------
// Object index: hazards / solids / orbs sorted by X so a lookup near the player
// is a binary search plus a short scan.  Positions and rects are re-read on
// every query, so moving objects stay correct as long as they don't travel
// further than the search margin from where they started.
// ---------------------------------------------------------------------------

struct Entry {
    float x;
    GameObject* obj;
};

std::vector<Entry> g_hazards;
std::vector<Entry> g_solids;
std::vector<Entry> g_orbs;

constexpr float kSearchMargin = 200.f;   // units either side of the query X

bool isHazardType(GameObjectType t) {
    return t == GameObjectType::Hazard || t == GameObjectType::AnimatedHazard;
}

bool isSolidType(GameObjectType t) {
    return t == GameObjectType::Solid || t == GameObjectType::Slope || t == GameObjectType::Breakable;
}

bool isOrbType(GameObjectType t) {
    switch (t) {
        case GameObjectType::YellowJumpRing:
        case GameObjectType::PinkJumpRing:
        case GameObjectType::GravityRing:
        case GameObjectType::GreenRing:
        case GameObjectType::RedJumpRing:
        case GameObjectType::CustomRing:
        case GameObjectType::DashRing:
        case GameObjectType::GravityDashRing:
        case GameObjectType::DropRing:
        case GameObjectType::SpiderOrb:
        case GameObjectType::TeleportOrb:
            return true;
        default:
            return false;
    }
}

void rebuildIndex(GJBaseGameLayer* layer) {
    g_hazards.clear();
    g_solids.clear();
    g_orbs.clear();
    if (!layer || !layer->m_objects) return;

    for (auto* obj : CCArrayExt<GameObject*>(layer->m_objects)) {
        if (!obj) continue;
        auto const type = obj->m_objectType;
        Entry e{obj->getPositionX(), obj};
        if (type == GameObjectType::Slope && obj->m_slopeIsHazard) {
            g_hazards.push_back(e);
        } else if (isHazardType(type)) {
            g_hazards.push_back(e);
        } else if (isSolidType(type)) {
            g_solids.push_back(e);
        } else if (isOrbType(type)) {
            g_orbs.push_back(e);
        }
    }

    auto byX = [](Entry const& a, Entry const& b) { return a.x < b.x; };
    std::sort(g_hazards.begin(), g_hazards.end(), byX);
    std::sort(g_solids.begin(), g_solids.end(), byX);
    std::sort(g_orbs.begin(), g_orbs.end(), byX);

    log::info("indexed {} hazards, {} solids, {} orbs", g_hazards.size(), g_solids.size(), g_orbs.size());
}

bool objectActive(GameObject* o) {
    return !(o->m_isDisabled || o->m_isGroupDisabled || o->m_isGroupDisabledTemp || o->m_isNoTouch);
}

// True if `pred` holds for any active object whose indexed X is within the
// margin of `x`.
template <class Pred>
bool anyNear(std::vector<Entry> const& v, float x, Pred&& pred) {
    auto it = std::lower_bound(v.begin(), v.end(), x - kSearchMargin,
        [](Entry const& e, float val) { return e.x < val; });
    for (; it != v.end() && it->x <= x + kSearchMargin; ++it) {
        GameObject* o = it->obj;
        if (!objectActive(o)) continue;
        if (pred(o)) return true;
    }
    return false;
}

CCRect shifted(CCRect r, CCPoint d) {
    r.origin.x += d.x;
    r.origin.y += d.y;
    return r;
}

// Position offset after k more ticks given per-tick velocity v and per-tick
// acceleration a:  k*v + a*k(k+1)/2
CCPoint extrapolate(CCPoint v, CCPoint a, int k) {
    float const s = 0.5f * static_cast<float>(k) * static_cast<float>(k + 1);
    return CCPoint{v.x * k + a.x * s, v.y * k + a.y * s};
}

// ---------------------------------------------------------------------------
// Player state helpers
// ---------------------------------------------------------------------------

bool touchingOrb(PlayerObject* p) {
    return p->m_touchingRings && p->m_touchingRings->count() > 0;
}

bool isHeldMode(PlayerObject* p) {
    return p->m_isShip || p->m_isDart;   // ship / wave: holding is the input
}

CCPoint velocityOf(Slot const& s) {
    return s.samples >= 2 ? s.lastDelta : CCPoint{0.f, 0.f};
}

CCPoint accelOf(Slot const& s) {
    return s.samples >= 3 ? s.lastAccel : CCPoint{0.f, 0.f};
}

// Does the player's extrapolated path overlap any orb within `maxTicks` ticks?
bool orbAhead(PlayerObject* p, CCPoint v, CCPoint a, int maxTicks) {
    if (std::abs(v.x) < 0.01f && std::abs(v.y) < 0.01f) return false;   // not moving: nothing "ahead"
    CCRect const base = p->getObjectRect();
    for (int k = 1; k <= maxTicks; ++k) {
        CCRect r = shifted(base, extrapolate(v, a, k));
        bool hit = anyNear(g_orbs, r.getMidX(), [&](GameObject* o) {
            return o->getObjectRect().intersectsRect(r);
        });
        if (hit) return true;
    }
    return false;
}

// Inner (hazard) hitbox overlaps a hazard after the next step.
bool wouldDieNextTick(PlayerObject* p, CCPoint delta) {
    CCRect inner = shifted(p->getObjectRect(g_cfg.hazardScale, g_cfg.hazardScale), delta);
    return anyNear(g_hazards, inner.getMidX(), [&](GameObject* o) {
        return o->getObjectRect().intersectsRect(inner);
    });
}

// Player is grounded now and nothing solid is under them after the next step.
bool wouldLeaveGroundNextTick(PlayerObject* p, CCPoint delta) {
    if (!p->m_isOnGround) return false;

    CCRect r = shifted(p->getObjectRect(), CCPoint{delta.x, 0.f});

    constexpr float kProbe = 3.f;
    CCRect probe = p->m_isUpsideDown
        ? CCRect(r.getMinX(), r.getMaxY(), r.size.width, kProbe)
        : CCRect(r.getMinX(), r.getMinY() - kProbe, r.size.width, kProbe);

    // The level floor (object space y = 0) counts as ground.
    if (!p->m_isUpsideDown && probe.getMinY() <= 0.5f) return false;

    bool stillGrounded = anyNear(g_solids, r.getMidX(), [&](GameObject* o) {
        if (o->m_isPassable) return false;
        return o->getObjectRect().intersectsRect(probe);
    });
    return !stillGrounded;
}

// Decide what to do with a fresh press for this player.
HoldKind classifyPress(PlayerObject* p, Slot const& s) {
    if (!p || p->m_isDead || p->m_isLocked) return HoldKind::None;
    if (touchingOrb(p)) return HoldKind::None;            // valid right now: let it through

    CCPoint const v = velocityOf(s);
    CCPoint const a = accelOf(s);

    // In the air (or in a held mode) with an orb coming up: hold for the orb.
    bool const airborne = !p->m_isOnGround || isHeldMode(p);
    if (airborne && orbAhead(p, v, a, g_cfg.maxDelay)) return HoldKind::Orb;

    // Grounded in a jump mode, opt-in edge buffering.
    if (g_cfg.groundEdge && p->m_isOnGround && !isHeldMode(p)) {
        if (wouldLeaveGroundNextTick(p, v)) return HoldKind::None;   // already the last tick
        return HoldKind::GroundEdge;
    }

    return HoldKind::None;
}

// ---------------------------------------------------------------------------
// Forwarding
// ---------------------------------------------------------------------------

int slotIndex(GJBaseGameLayer* layer, PlayerObject* p) {
    return (layer->m_player2 && p == layer->m_player2) ? 1 : 0;
}

// Player a given input flag controls.  In single-player mode both control
// sets drive player 1, same as the game does.
PlayerObject* targetPlayer(GJBaseGameLayer* layer, bool isPlayer1) {
    if (isPlayer1 || !layer->m_gameState.m_isDualMode) return layer->m_player1;
    return layer->m_player2;
}

void forwardButton(GJBaseGameLayer* layer, bool down, bool isPlayer1) {
    g_passthrough = true;
    layer->handleButton(down, static_cast<int>(PlayerButton::Jump), isPlayer1);
    g_passthrough = false;
}

void updateIndicator(PlayLayer* pl);

void firePending(GJBaseGameLayer* layer, Slot& s, FireReason reason) {
    int const delay = std::max(0, s.ticksHeld - 1);
    forwardButton(layer, true, s.viaPlayer1Input);
    s.pending = false;
    s.kind = HoldKind::None;

    if (s.releaseQueued) {
        s.releaseCountdown = std::max(1, s.releaseDelay);
        s.releaseQueued = false;
    }

    g_lastReason = reason;
    g_lastDelay = delay;
    if (delay > 0) {
        g_attemptTainted = true;
        g_correctedCount += 1;
        g_correctedTicks += delay;
    }
    log::info("fired held press: reason={} delay={} tick(s)", reasonName(reason), delay);
    if (auto* pl = PlayLayer::get()) updateIndicator(pl);
}

void resetAttemptState(bool clearTaint) {
    for (auto& s : g_slots) s.clear();
    g_correctedCount = 0;
    g_correctedTicks = 0;
    g_lastReason = FireReason::None;
    g_lastDelay = 0;
    if (clearTaint) g_attemptTainted = false;
}

bool isLivePlayLayer(GJBaseGameLayer* layer) {
    auto* pl = PlayLayer::get();
    return pl && static_cast<GJBaseGameLayer*>(pl) == layer;
}

} // namespace

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------

class $modify(FPBBaseLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        if (g_passthrough || !g_cfg.enabled || button != static_cast<int>(PlayerButton::Jump)
            || !isLivePlayLayer(this)) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        PlayerObject* p = targetPlayer(this, isPlayer1);
        if (!p) return GJBaseGameLayer::handleButton(down, button, isPlayer1);

        int const idx = slotIndex(this, p);
        if (idx == 1 && !g_cfg.player2) return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        Slot& s = g_slots[idx];

        if (!down) {
            if (s.pending) {
                // Press hasn't gone out yet, so the release can't either; remember
                // how long the button was held so the forwarded tap keeps its length.
                s.releaseQueued = true;
                s.releaseDelay = s.ticksHeld;
                return;
            }
            if (s.releaseCountdown > 0) {
                // A delayed release is already scheduled; this one is redundant.
                return;
            }
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        // down == true
        if (s.pending) return;   // already holding a press for this player

        if (s.releaseCountdown > 0) {
            // The previous tap's release is still scheduled; flush it first so
            // the game sees press -> release -> press in order.
            forwardButton(this, false, s.viaPlayer1Input);
            s.releaseCountdown = 0;
        }

        HoldKind const kind = classifyPress(p, s);
        if (kind == HoldKind::None) {
            g_lastReason = FireReason::Immediate;
            g_lastDelay = 0;
            if (auto* pl = PlayLayer::get()) updateIndicator(pl);
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        s.pending = true;
        s.kind = kind;
        s.viaPlayer1Input = isPlayer1;
        s.ticksHeld = 0;
        s.releaseQueued = false;
        s.releaseDelay = 0;
        log::info("holding press ({})", kind == HoldKind::Orb ? "orb ahead" : "ground edge");
    }
};

class $modify(FPBPlayer, PlayerObject) {
    void update(float dt) {
        auto* pl = PlayLayer::get();
        if (!pl || !g_cfg.enabled || (this != pl->m_player1 && this != pl->m_player2)) {
            return PlayerObject::update(dt);
        }

        Slot& s = g_slots[slotIndex(pl, this)];
        CCPoint const pos = this->getPosition();
        CCPoint const delta = s.samples >= 1 ? (pos - s.lastPos) : CCPoint{0.f, 0.f};

        // A release we promised after a forwarded tap.
        if (s.releaseCountdown > 0) {
            if (--s.releaseCountdown == 0) forwardButton(pl, false, s.viaPlayer1Input);
        }

        if (s.pending) {
            s.ticksHeld += 1;
            FireReason fire = FireReason::None;

            if (this->m_isDead) {
                s.pending = false;                                   // too late, drop it
                s.kind = HoldKind::None;
            } else if (s.ticksHeld - 1 >= g_cfg.maxDelay) {
                fire = FireReason::Cap;
            } else if (wouldDieNextTick(this, delta)) {
                fire = FireReason::Hazard;
            } else if (s.kind == HoldKind::Orb) {
                if (touchingOrb(this)) fire = FireReason::Orb;       // the tick we were waiting for
            } else if (s.kind == HoldKind::GroundEdge) {
                if (touchingOrb(this)) fire = FireReason::Orb;
                else if (!this->m_isOnGround) fire = FireReason::Left;   // left the ground some other way
                else if (wouldLeaveGroundNextTick(this, delta)) fire = FireReason::Edge;
            }

            if (fire != FireReason::None) firePending(pl, s, fire);
        }

        // Advance motion samples.
        if (s.samples >= 2) s.lastAccel = delta - s.lastDelta;
        if (s.samples >= 1) s.lastDelta = delta;
        s.lastPos = pos;
        if (s.samples < 3) s.samples += 1;

        PlayerObject::update(dt);
    }
};

class $modify(FPBPlayLayer, PlayLayer) {
    struct Fields {
        CCLabelBMFont* indicator = nullptr;
    };

    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        reloadConfig();
        resetAttemptState(true);
        rebuildIndex(this);

        if (g_cfg.showIndicator) {
            auto* label = CCLabelBMFont::create("", "chatFont.fnt");
            label->setAnchorPoint({0.f, 0.f});
            label->setScale(0.6f);
            label->setOpacity(160);
            label->setPosition({6.f, 6.f});
            label->setZOrder(1000);
            label->setID("indicator"_spr);
            this->addChild(label);
            m_fields->indicator = label;
            updateIndicator(this);
        }
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        // In practice mode a reset is just a checkpoint respawn, so keep the
        // taint; a normal-mode reset is a brand new attempt.
        resetAttemptState(!m_isPracticeMode);
        updateIndicator(this);
    }

    void levelComplete() {
        if (g_cfg.enabled && g_cfg.safeMode && g_attemptTainted) {
            bool const wasTest = m_isTestMode;
            m_isTestMode = true;             // completion is not recorded
            PlayLayer::levelComplete();
            m_isTestMode = wasTest;
            return;
        }
        PlayLayer::levelComplete();
    }

    void onQuit() {
        PlayLayer::onQuit();
        resetAttemptState(true);
        g_hazards.clear();
        g_solids.clear();
        g_orbs.clear();
    }
};

class $modify(FPBLevel, GJGameLevel) {
    void savePercentage(int percent, bool isPracticeMode, int clicks, int attempts, bool isChkValid) {
        if (g_cfg.enabled && g_cfg.safeMode && g_attemptTainted && PlayLayer::get()) {
            log::info("safe mode: not saving {}% for this attempt", percent);
            return;
        }
        GJGameLevel::savePercentage(percent, isPracticeMode, clicks, attempts, isChkValid);
    }
};

namespace {

void updateIndicator(PlayLayer* pl) {
    auto* self = static_cast<FPBPlayLayer*>(pl);
    auto* label = self->m_fields->indicator;
    if (!label) return;

    if (!g_cfg.enabled) {
        label->setString("FPB off");
        return;
    }
    std::string text = fmt::format("FPB  fixed {}  (+{} ticks)  last: {} +{}",
        g_correctedCount, g_correctedTicks, reasonName(g_lastReason), g_lastDelay);
    if (g_cfg.safeMode && g_attemptTainted) text += "  [safe]";
    label->setString(text.c_str());
}

} // namespace

$on_mod(Loaded) {
    reloadConfig();
    listenForAllSettingChanges([](std::string_view, std::shared_ptr<SettingV3>) {
        reloadConfig();
        if (auto* pl = PlayLayer::get()) updateIndicator(pl);
    });
}
