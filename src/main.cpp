// Frame Perfect Buffer
//
// Holds back an early jump press and forwards it on the last tick where it can
// still do its job.  "Tick" here means one physics step: GD 2.2 runs physics at
// a fixed 240 steps per second regardless of the render FPS, which is exactly the
// "240 fps frame" people mean when they talk about frame-perfect inputs.
//
// The mod cannot know in advance which tick a level designer intended; what it
// can do is detect, one tick ahead, the moment an input is about to stop being
// possible:
//
//   * ground edge  - the player is grounded now, and after the next step there
//                    is no solid under them any more  -> last tick to jump
//   * orb exit     - the player is touching an orb now, and after the next step
//                    they no longer overlap it         -> last tick to hit it
//   * death        - the player's inner hitbox overlaps a hazard after the next
//                    step                              -> jump now or die
//
// While none of those is true the press is held, up to a hard cap
// ("Max delay").  When the cap runs out the press fires anyway, so an ordinary
// early jump is delayed by at most that many ticks.  Keep the cap small: every
// jump inside the window is moved to its last viable tick, which is only what
// you want on inputs that were going to be late-or-die anyway.
//
// Safe mode: if any press in the attempt was actually delayed, progress and
// completion for that attempt are not saved.

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJGameLevel.hpp>

#include <algorithm>
#include <vector>

using namespace geode::prelude;

namespace {

// ---------------------------------------------------------------------------
// Settings (cached so the per-tick path never touches the settings system)
// ---------------------------------------------------------------------------

struct Config {
    bool enabled = true;
    int maxDelay = 8;              // ticks
    float hazardScale = 0.3f;      // inner hitbox size relative to full hitbox
    bool bufferUfoSwing = false;
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
    g_cfg.bufferUfoSwing = mod->getSettingValue<bool>("buffer-ufo-swing");
    g_cfg.player2 = mod->getSettingValue<bool>("player-2");
    g_cfg.safeMode = mod->getSettingValue<bool>("safe-mode");
    g_cfg.showIndicator = mod->getSettingValue<bool>("show-indicator");
}

// ---------------------------------------------------------------------------
// Per-player buffer state
// ---------------------------------------------------------------------------

struct Slot {
    bool pending = false;          // a press has been received and not yet forwarded
    bool viaPlayer1Input = true;   // the isPlayer1 flag the press arrived with
    int ticksHeld = 0;             // physics ticks since the press arrived
    bool releaseQueued = false;    // the matching release arrived while pending
    int releaseDelay = 0;          // ticks between the press and that release
    int releaseCountdown = 0;      // >0: forward a release after this many ticks

    CCPoint lastPos{};             // position at the start of the previous tick
    bool hasLastPos = false;

    void clear() { *this = Slot{}; }
};

Slot g_slots[2];                   // 0 = player 1, 1 = player 2
bool g_passthrough = false;        // true while we call the real handleButton ourselves

// Per-attempt bookkeeping
int g_correctedCount = 0;
int g_correctedTicks = 0;
bool g_attemptTainted = false;     // some press in this attempt was actually delayed

// ---------------------------------------------------------------------------
// Object index: hazards / solids / orbs sorted by X so a lookup near the player
// is a binary search plus a short scan instead of a walk over every object.
// Positions are re-read on every query, so moving objects stay correct as long
// as they don't travel further than the search margin from where they started.
// ---------------------------------------------------------------------------

struct Entry {
    float x;
    GameObject* obj;
};

std::vector<Entry> g_hazards;
std::vector<Entry> g_solids;
std::vector<Entry> g_orbs;

constexpr float kSearchMargin = 150.f;   // units either side of the player's X

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

// Returns true if `pred` is true for any active object whose indexed X is within
// the margin of `x`.
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

// ---------------------------------------------------------------------------
// One-tick-ahead checks
// ---------------------------------------------------------------------------

// Inner (hazard) hitbox overlaps a hazard after the next step.
bool wouldDieNextTick(PlayerObject* p, CCPoint delta) {
    CCRect inner = shifted(p->getObjectRect(g_cfg.hazardScale, g_cfg.hazardScale), delta);
    float const nx = inner.getMidX();
    return anyNear(g_hazards, nx, [&](GameObject* o) {
        return o->getObjectRect().intersectsRect(inner);
    });
}

// Player is grounded now and nothing solid is under them after the next step.
bool wouldLeaveGroundNextTick(PlayerObject* p, CCPoint delta) {
    if (!p->m_isOnGround) return false;

    // Only horizontal motion matters for "is there still floor under me"; when
    // grounded the vertical delta is ~0 anyway.
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

// Player is overlapping an orb now and will overlap none of them after the next step.
bool wouldLeaveOrbNextTick(PlayerObject* p, CCPoint delta) {
    if (!p->m_touchingRings || p->m_touchingRings->count() == 0) return false;

    CCRect r = shifted(p->getObjectRect(), delta);
    for (auto* ring : CCArrayExt<GameObject*>(p->m_touchingRings)) {
        if (ring && ring->getObjectRect().intersectsRect(r)) return false;
    }
    return true;
}

bool touchingOrb(PlayerObject* p) {
    return p->m_touchingRings && p->m_touchingRings->count() > 0;
}

// Can a press for this player be usefully held back right now?
bool bufferable(PlayerObject* p) {
    if (!p || p->m_isDead || p->m_isLocked) return false;
    if (p->m_isShip || p->m_isDart) return false;             // held modes: nothing to buffer
    if ((p->m_isBird || p->m_isSwing) && !g_cfg.bufferUfoSwing) return false;
    if (touchingOrb(p)) return true;
    if (p->m_isBird || p->m_isSwing) return true;              // released by the death check / cap only
    return p->m_isOnGround;                                    // cube / robot / ball / spider
}

// Should a held press fire on this tick?
bool shouldFireNow(PlayerObject* p, CCPoint delta) {
    if (wouldDieNextTick(p, delta)) return true;
    if (wouldLeaveOrbNextTick(p, delta)) return true;
    if (wouldLeaveGroundNextTick(p, delta)) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Helpers
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

void firePending(GJBaseGameLayer* layer, Slot& s) {
    int const delay = std::max(0, s.ticksHeld - 1);
    forwardButton(layer, true, s.viaPlayer1Input);
    s.pending = false;

    if (s.releaseQueued) {
        s.releaseCountdown = std::max(1, s.releaseDelay);
        s.releaseQueued = false;
    }

    if (delay > 0) {
        g_attemptTainted = true;
        g_correctedCount += 1;
        g_correctedTicks += delay;
        log::debug("fired a buffered press {} tick(s) late", delay);
        if (auto* pl = PlayLayer::get()) updateIndicator(pl);
    }
}

void resetAttemptState(bool clearTaint) {
    for (auto& s : g_slots) s.clear();
    g_correctedCount = 0;
    g_correctedTicks = 0;
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

        if (!bufferable(p)) return GJBaseGameLayer::handleButton(down, button, isPlayer1);

        CCPoint delta = s.hasLastPos ? (p->getPosition() - s.lastPos) : CCPoint{0.f, 0.f};
        if (shouldFireNow(p, delta)) {
            // This already is the last viable tick: no delay needed.
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        s.pending = true;
        s.viaPlayer1Input = isPlayer1;
        s.ticksHeld = 0;
        s.releaseQueued = false;
        s.releaseDelay = 0;
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
        CCPoint const delta = s.hasLastPos ? (pos - s.lastPos) : CCPoint{0.f, 0.f};

        // A release we promised after a forwarded tap.
        if (s.releaseCountdown > 0) {
            if (--s.releaseCountdown == 0) forwardButton(pl, false, s.viaPlayer1Input);
        }

        if (s.pending) {
            s.ticksHeld += 1;
            bool fire = false;

            if (s.ticksHeld - 1 >= g_cfg.maxDelay) {
                fire = true;                         // out of patience
            } else if (this->m_isDead) {
                s.pending = false;                   // too late, drop it
            } else if (!bufferable(this) && !this->m_isDead) {
                fire = true;                         // left the situation we were waiting on
            } else if (shouldFireNow(this, delta)) {
                fire = true;                         // last viable tick
            }

            if (fire) firePending(pl, s);
        }

        s.lastPos = pos;
        s.hasLastPos = true;

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
    std::string text = fmt::format("FPB  fixed {}  (+{} ticks)", g_correctedCount, g_correctedTicks);
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
