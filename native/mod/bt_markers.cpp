// Round 30 BT S1: the partner's BT trouble markers (see bt_markers.hpp).
#include "bt_markers.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "bt_rules.hpp"
#include "game_actions.hpp"
#include "log.hpp"
#include "nametags.hpp"
#include "overlay/overlay_tags.hpp"

namespace st::btmarks {
namespace {

namespace R = btrules;
namespace tags = overlay::tags;

constexpr double kGazerOverM = 2.6;     // the diamond over a gazer's position (its root)
constexpr double kCatcherOverM = 3.0;   // the catcher's mark over its root
constexpr double kTarGroundM = 0.15;    // the ring just above the partner's feet
constexpr float kTarRadiusM = 6.0f;

bool g_on = false;
R::MarkerBook g_book;
bool g_published = false;   // the last frame published had marks (a hidden frame goes out once)
char g_hidden[64] = "start";
unsigned long g_seen_drawn = 0;
double g_next_log_ms = 0;
bool g_logged_any = false;

void SetHidden(const char* why) {
    if (std::strcmp(why, g_hidden) == 0) return;
    std::snprintf(g_hidden, sizeof(g_hidden), "%s", why);
}

void Hide(const char* why) {
    SetHidden(why);
    if (!g_published) return;
    g_published = false;
    tags::BtFrame f;  // show = false: the overlay stops drawing the marks at once
    tags::PublishBt(f);
}

bool RootOf(game::Entity* e, double (&pos)[3]) {
    game::WorldTransform xf{};
    if (!e || !game::TryGetTransform(e, &xf)) return false;
    pos[0] = xf.pos.x, pos[1] = xf.pos.y, pos[2] = xf.pos.z;
    return R::PosSane(pos);
}

void PutMark(std::uint64_t key, std::uint8_t kind, const double (&pos)[3], float hp, bool gold, bool test, double now_ms) {
    if (!g_on || !R::PosSane(pos)) return;
    R::MarkEntry m;
    m.key = key, m.kind = kind, m.hp = hp, m.gold = gold, m.test = test, m.seen_ms = now_ms;
    std::memcpy(m.pos, pos, sizeof(m.pos));
    g_book.Put(m);
}

}  // namespace

void Init(const Config& config) noexcept {
    g_on = config.bt_markers && config.bt_trouble && config.bt_sync > 0;
    Log("[bt] markers %s", g_on ? "on: the partner's BT trouble drawn as small marks in the world (no text)"
                                : config.bt_markers ? "off (bt_trouble=0 or bt_sync=0)" : "off (bt_markers=0)");
}

bool On() noexcept { return g_on; }

void Gazer(std::uint32_t loc, const double (&pos)[3], bool test, double now_ms) noexcept {
    PutMark(R::GazerMarkKey(loc), R::kMarkGazer, pos, -1, false, test, now_ms);
}

void DropGazer(std::uint32_t loc) noexcept { g_book.Drop(R::GazerMarkKey(loc)); }

void Tar(const double (&pos)[3], bool gold, bool test, double now_ms) noexcept {
    PutMark(R::kMarkKeyTar, R::kMarkTar, pos, -1, gold, test, now_ms);
}

void Catcher(const double (&pos)[3], float hp_pct, double now_ms) noexcept {
    PutMark(R::kMarkKeyCatcher, R::kMarkCatcher, pos, hp_pct, false, false, now_ms);
}

void ClearTrouble() noexcept {
    g_book.DropKind(R::kMarkTar);
    g_book.DropKind(R::kMarkCatcher);
}

void Tick(game::Entity* local, bool local_active, game::Entity* partner, bool partner_fresh, double now_ms) noexcept {
    if (!g_on) return;
    g_book.Expire(now_ms);
    if (!g_book.n) return Hide("no marks");
    if (!local) return Hide("no local player");
    std::uint64_t flags = 0;
    std::uint32_t load = 0;
    if (!game::ReadStopGates(&flags, &load)) return Hide("game state unreadable");
    if (flags & game::kStoppingGameEventMask) return Hide("a menu, cutscene or scripted scene");
    if (load) return Hide("loading");
    if (!local_active) return Hide("Sam asleep");
    tags::BtFrame f;
    f.view = nametags::CameraViewObject(local);
    if (!f.view) return Hide("no camera view");
    double body[3];
    const bool body_ok = partner && partner_fresh && RootOf(partner, body);
    for (unsigned i = 0; i < g_book.n && f.count < tags::kMaxBtMarks; ++i) {
        const R::MarkEntry& m = g_book.e[i];
        tags::BtMark& b = f.marks[f.count];
        std::memcpy(b.anchor, m.pos, sizeof(b.anchor));
        if (m.kind == R::kMarkGazer) {
            b.kind = tags::kBtGazer;
            b.anchor[2] += kGazerOverM;
        } else if (m.kind == R::kMarkTar) {
            b.kind = tags::kBtTar;
            if (body_ok) std::memcpy(b.anchor, body, sizeof(b.anchor));
            b.anchor[2] += kTarGroundM;
            b.radius = kTarRadiusM;
            b.gold = m.gold;
        } else if (m.kind == R::kMarkCatcher) {
            b.kind = tags::kBtCatcher;
            b.anchor[2] += kCatcherOverM;
            b.hp = m.hp;
        } else {
            continue;
        }
        ++f.count;
    }
    if (!f.count) return Hide("no marks");
    f.show = true;
    SetHidden("");
    g_published = true;
    tags::PublishBt(f);
    // evidence (hidden runs): the overlay's count, the first time it drew, then every 5 s while it draws
    const tags::Stats s = tags::GetStats();
    if (s.bt_drawn_frames != g_seen_drawn && (!g_logged_any || now_ms >= g_next_log_ms)) {
        g_seen_drawn = s.bt_drawn_frames;
        g_logged_any = true;
        g_next_log_ms = now_ms + 5000.0;
        Log("[bt] markers: %u drawn (gazer %u, tar %u, catcher %u); %u kept", s.bt_last_drawn, s.bt_last_gazer, s.bt_last_tar, s.bt_last_catcher,
            g_book.n);
    }
}

}  // namespace st::btmarks
