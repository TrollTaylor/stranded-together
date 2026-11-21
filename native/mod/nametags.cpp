#include "nametags.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "game_actions.hpp"
#include "link.hpp"
#include "log.hpp"
#include "nametag_math.hpp"
#include "overlay/overlay.hpp"
#include "overlay/overlay_tags.hpp"
#include "partner_source.hpp"
#include "pose_wire.hpp"
#include "real_body.hpp"
#include "steam_owner.hpp"

namespace st::nametags {
namespace {

namespace mem = partner::mem;

// ---------------------------------------------------------------- ds.exe (image base 0x140000000)
// The camera: the local Player's view object (Player + 0xD0) keeps a copy of the camera's
// ViewFrustumInfo, written by its per-frame update (vtable slot 2, 0x14361AA50): position, the
// three axes, near/far and the frustum extents at the near plane. docs/real-partner-body.md 10.1.
constexpr std::uintptr_t kSamPlayer = 0x560;          // DSPlayerEntity -> Player* (GetPlayer 0x1427115B0)
constexpr std::uintptr_t kPlayerView = 0xD0;          // Player -> camera view object
constexpr std::uintptr_t kViewVtable = 0x3F15208;     // its vtable (ctor 0x143614730)
constexpr std::uintptr_t kViewUpdate = 0x361AA50;     // vtable slot 2: the per-frame update
constexpr std::uintptr_t kViewSnapshot = 0x28;        // ViewFrustumInfo copy: 0x28 .. 0x84
constexpr std::size_t kViewSnapshotSize = 0x5C;

struct Check {
    const char* name;
    std::uintptr_t rva;
    const char* hex;
};
const Check kChecks[] = {
    // DSPlayerEntity::GetPlayer: mov rax,[rcx+0x560]; ret
    {"DSPlayerEntity::GetPlayer", 0x27115B0, "488B8160050000C3"},
    // Player -> view (+0xD0), then the view's viewport count (+0x5F8)
    {"Player view getter", 0x23C6140, "488B81D00000004885C074078B80F8050000C3"},
    // DSCameraInterface GetCameraPosition: FindPlayer, [P+0xD0], the view's position getter
    {"GetCameraPosition", 0x26E8FC2, "4885C0742B488B88D00000004885C9741F488D542440E863CDCDFF"},
    // the view update: vt+0x48 (blend into the camera), camera vt+0x98 (GetViewFrustum), the copy
    // into the view at +0x28 (pos), +0x40 (axes), +0x68 .. +0x80 (near, far, top, bottom, left,
    // right, projection mode)
    {"view update frustum copy", 0x361AAC6,
     "FF5748498B4E204C8D442450C5F857C0488D542460488B01C5F811442450FF9098000000488BC8498D5640C5F81000C4C178114628C5FB1048104883C018"
     "C4C17B114E38483BD0740EC5FC1000C5FC11028B40208942208B4140418946688B41444189466C8B4148418946708B414C418946748B4150418946788B41"
     "544189467C8B4158498BCE41898680000000"},
};

// The tag sits kOverHeadM above the body's head joint (joint 31; 1.59 m over the feet when standing,
// so 1.95 m like the earlier root anchor for a standing Sam), following crouches, leans and shorter
// bodies. Without a readable head (the hologram, a marker) it is kTagHeightM above the root (its
// feet; run 092833: 2.05 sat ~0.3 m high).
constexpr double kOverHeadM = 0.36;
constexpr double kTagHeightM = 1.95;

bool HexAt(std::uintptr_t va, const char* hex) {
    std::uint8_t got[192];
    const std::size_t n = std::strlen(hex) / 2;
    if (n > sizeof(got) || !mem::Read(va, got, n)) return false;
    for (std::size_t i = 0; i < n; ++i) {
        const char b[3] = {hex[2 * i], hex[2 * i + 1], 0};
        if (got[i] != static_cast<std::uint8_t>(std::strtoul(b, nullptr, 16))) return false;
    }
    return true;
}

// ---------------------------------------------------------------- state (game thread unless noted)
Config g_cfg;
bool g_net = false;
bool g_on = false;           // name_tags=1 and a role with a partner (runtime switch: action name_tags 3)
bool g_self_test = false;    // action name_tags 1: a tag above our own Sam
int g_resolved = 0;          // 0 not yet, 1 ok, -1 a byte check failed
std::uintptr_t g_view_vtable = 0;  // also read by ReadView (render thread); set once before the first publish
char g_own[wire::kMaxNameBytes + 1] = {};
double g_next_name_ms = 0;
char g_last_label[96] = {};
int g_last_head = -1;  // the anchor kind last logged
char g_last_hidden[96] = "start";
std::uint32_t g_logged_peer_hash = 0;
std::uint32_t g_logged_peer_gen = 0xFFFFFFFFu;
unsigned g_bad_view_logs = 0;

bool Resolve() {
    if (g_resolved) return g_resolved > 0;
    const std::uintptr_t base = mem::Base();
    for (const Check& c : kChecks) {
        if (!HexAt(base + c.rva, c.hex)) {
            Log("[tags] byte check failed: %s at 0x%llX differs from this ds.exe build: name tags off", c.name,
                static_cast<unsigned long long>(0x140000000ull + c.rva));
            g_resolved = -1;
            return false;
        }
    }
    std::uintptr_t slot = 0;
    if (!mem::Rd(base + kViewVtable + 0x10, slot) || slot != base + kViewUpdate) {
        Log("[tags] byte check failed: the camera view vtable slot 2 is 0x%llX, not 0x%llX: name tags off",
            static_cast<unsigned long long>(slot), static_cast<unsigned long long>(base + kViewUpdate));
        g_resolved = -1;
        return false;
    }
    g_view_vtable = base + kViewVtable;
    g_resolved = 1;
    Log("[tags] camera view checks passed (%zu sites + the view vtable)", sizeof(kChecks) / sizeof(kChecks[0]));
    return true;
}

// The camera view object of the local player, or 0.
std::uintptr_t ViewOf(game::Entity* local) {
    if (!local || g_resolved <= 0) return 0;
    const std::uintptr_t player = mem::Q(reinterpret_cast<std::uintptr_t>(local) + kSamPlayer);
    const std::uintptr_t view = player ? mem::Q(player + kPlayerView) : 0;
    std::uintptr_t vt = 0;
    if (!view || !mem::Rd(view, vt)) return 0;
    if (vt != g_view_vtable) {
        if (g_bad_view_logs < 5) {
            ++g_bad_view_logs;
            Log("[tags] the player's view object has vtable 0x%llX (expected 0x%llX): tags hidden", static_cast<unsigned long long>(vt),
                static_cast<unsigned long long>(g_view_vtable));
        }
        return 0;
    }
    return view;
}

// Render thread (the overlay, inside Present) and game thread. Fault-free: the vtable is checked
// first, and the snapshot is read twice and must match (the game writes it non-atomically).
bool ReadView(std::uintptr_t view, nametag::View* out) noexcept {
    const std::uintptr_t want_vt = g_view_vtable;
    std::uintptr_t vt = 0;
    if (!view || !want_vt || !mem::Rd(view, vt) || vt != want_vt) return false;
    std::uint8_t a[kViewSnapshotSize], b[kViewSnapshotSize];
    bool same = false;
    for (int attempt = 0; attempt < 3 && !same; ++attempt)
        same = mem::Read(view + kViewSnapshot, a, sizeof(a)) && mem::Read(view + kViewSnapshot, b, sizeof(b)) &&
               std::memcmp(a, b, sizeof(a)) == 0;
    if (!same) return false;
    nametag::View v;
    std::memcpy(v.pos, a + 0x00, sizeof(v.pos));     // view +0x28
    std::memcpy(v.axis, a + 0x18, sizeof(v.axis));   // view +0x40
    std::memcpy(&v.near_plane, a + 0x40, 4);         // view +0x68
    std::memcpy(&v.far_plane, a + 0x44, 4);          // view +0x6C
    std::memcpy(&v.top, a + 0x48, 4);                // view +0x70
    std::memcpy(&v.right, a + 0x54, 4);              // view +0x7C
    std::uint32_t mode = 0;
    std::memcpy(&mode, a + 0x58, 4);                 // view +0x80: 0 perspective, 1 orthographic
    v.perspective = mode == 0;
    if (!nametag::ViewUsable(v)) return false;
    *out = v;
    return true;
}

bool ReadPos(game::Entity* e, double (&pos)[3]) {
    game::WorldTransform wt{};
    if (!e || !game::TryGetTransform(e, &wt)) return false;
    pos[0] = wt.pos.x, pos[1] = wt.pos.y, pos[2] = wt.pos.z;
    return std::isfinite(pos[0]) && std::isfinite(pos[1]) && std::isfinite(pos[2]);
}

// The tag's anchor: over the head joint when it reads, else over the root.
bool AnchorOver(game::Entity* e, double (&pos)[3], bool* head) {
    *head = realbody::HeadWorld(e, pos);
    if (*head) {
        pos[2] += kOverHeadM;
        return true;
    }
    if (!ReadPos(e, pos)) return false;
    pos[2] += kTagHeightM;
    return true;
}

// Our own name: session.ini name= wins, else the Steam persona (known once Steam is ready).
void UpdateOwnName(double now) {
    if (now < g_next_name_ms) return;
    g_next_name_ms = now + 1000;
    const char* src = !g_cfg.player_name.empty() ? g_cfg.player_name.c_str() : steam::PersonaName();
    char clean[sizeof(g_own)];
    wire::SanitizeName(src, std::strlen(src), clean, sizeof(clean));
    if (std::strcmp(clean, g_own) != 0) {
        std::memcpy(g_own, clean, sizeof(g_own));
        Log("[tags] our name: %zu B fnv %08X (%s)%s", std::strlen(g_own), nametag::NameHash(g_own),
            !g_cfg.player_name.empty() ? "session.ini name=" : "Steam", g_net && g_on ? ", sent to the partner (STN1)" : "");
    }
    if (g_net) link::SetLocalName(g_own, g_on && g_own[0]);
}

void SetHidden(const char* why) {
    if (std::strcmp(why, g_last_hidden) == 0) return;
    std::snprintf(g_last_hidden, sizeof(g_last_hidden), "%s", why);
    if (why[0]) Log("[tags] hidden: %s", why);
    else Log("[tags] shown");
}

void Hide(const char* why) {
    SetHidden(why);
    overlay::tags::Frame f;  // show = false: the overlay stops drawing at once
    overlay::tags::Publish(f);
}

}  // namespace

void Init(const Config& config) noexcept {
    g_cfg = config;
    g_net = config.role == Role::host || config.role == Role::guest;
    const bool has_partner = g_net || config.role == Role::echo;
    g_on = config.name_tags && has_partner;
    overlay::tags::SetViewReader(&ReadView);
    if (!config.name_tags) Log("[tags] name tags off (name_tags=0): no names are drawn or sent");
    else if (!has_partner) Log("[tags] name tags: no partner in role %s (action name_tags 1 shows a test tag)", RoleName(config.role));
    else
        Log("[tags] name tags on: the partner's name is drawn above their head%s%s", g_net ? "; our name is sent (STN1)" : " (echo: our own name)",
            overlay::Installed() ? "" : "; but the overlay is not installed, nothing can be drawn");
}

void Tick(game::Entity* local, bool local_active, game::Entity* partner, bool partner_fresh, double now) noexcept {
    if (!g_on && !g_self_test) {
        if (g_net && now >= g_next_name_ms) {  // keep the link told that we send nothing
            g_next_name_ms = now + 1000;
            link::SetLocalName("", false);
        }
        if (g_last_hidden[0] != 'o') Hide("off");
        return;
    }
    UpdateOwnName(now);
    if (!local) return Hide("no local player");
    if (!Resolve()) return Hide("byte check failed");

    // Gates: the world is not being played (menus, cutscenes, loads, map, ...).
    std::uint64_t flags = 0;
    std::uint32_t load = 0;
    if (!game::ReadStopGates(&flags, &load)) return Hide("game state unreadable");
    if (flags & game::kStoppingGameEventMask) {
        char names[96];
        game::DescribeGameStateFlags(flags & game::kStoppingGameEventMask, names, sizeof(names));
        char why[96];
        std::snprintf(why, sizeof(why), "game state %s", names);
        return Hide(why);
    }
    if (load) return Hide("loading");
    if (!local_active) return Hide("player asleep (load, cutscene or knot space)");

    overlay::tags::Frame f;
    f.view = ViewOf(local);
    if (!f.view) return Hide("no camera view");

    char label[96] = {};
    double pos[3];
    bool head = false;
    if (g_self_test) {
        if (!AnchorOver(local, pos, &head)) return Hide("own position unreadable");
        nametag::ComposeLabel(g_own[0] ? g_own : "You", nullptr, false, false, label, sizeof(label));
        std::strncat(label, " (test)", sizeof(label) - std::strlen(label) - 1);
    } else {
        if (!partner || !partner_fresh) return Hide("no partner shown");
        // A real body the game keeps hidden (entity flag 0x2 clear) gets no tag over nothing.
        std::uint64_t pf = 0;
        if (partner == realbody::LiveEntity() && (!partner::ReadFlags(partner, &pf) || !(pf & 0x2))) return Hide("the partner body is hidden");
        if (!AnchorOver(partner, pos, &head)) return Hide("partner position unreadable");
        if (g_net) {
            char peer[wire::kMaxNameBytes + 1] = {};
            std::uint32_t gen = 0;
            bool from_host = false;
            if (link::PeerName(peer, sizeof(peer), &gen, &from_host)) {
                const std::uint32_t h = nametag::NameHash(peer);
                if (h != g_logged_peer_hash || gen != g_logged_peer_gen) {
                    g_logged_peer_hash = h, g_logged_peer_gen = gen;
                    Log("[tags] partner name: %zu B fnv %08X (session #%u, sent by the %s)", std::strlen(peer), h, gen,
                        from_host ? "host" : "guest");
                }
            }
            nametag::ComposeLabel(peer, g_own, g_cfg.role == Role::guest, false, label, sizeof(label));
        } else {
            nametag::ComposeLabel(nullptr, g_own, false, true, label, sizeof(label));
        }
    }
    if ((head ? 1 : 0) != g_last_head) {
        g_last_head = head ? 1 : 0;
        Log("[tags] anchor: %s", head ? "over the head joint" : "over the root (no readable head)");
    }
    f.show = true;
    f.count = 1;
    std::memcpy(f.tags[0].anchor, pos, sizeof(pos));
    std::snprintf(f.tags[0].text, sizeof(f.tags[0].text), "%s", label);
    if (std::strcmp(label, g_last_label) != 0) {
        std::snprintf(g_last_label, sizeof(g_last_label), "%s", label);
        Log("[tags] label: %zu B fnv %08X", std::strlen(label), nametag::NameHash(label));
    }
    SetHidden("");
    overlay::tags::Publish(f);
}

bool CameraForward(game::Entity* local, double (&fwd)[2]) noexcept {
    if (!local || !Resolve()) return false;
    nametag::View v;
    if (!ReadView(ViewOf(local), &v)) return false;
    const double x = v.axis[1][0], y = v.axis[1][1], n = std::sqrt(x * x + y * y);
    if (!(n > 0.1)) return false;
    fwd[0] = x / n, fwd[1] = y / n;
    return true;
}

bool CameraView(game::Entity* local, double (&pos)[3], double (&fwd)[3]) noexcept {
    if (!local || !Resolve()) return false;
    nametag::View v;
    if (!ReadView(ViewOf(local), &v)) return false;
    const double x = v.axis[1][0], y = v.axis[1][1], z = v.axis[1][2], n = std::sqrt(x * x + y * y + z * z);
    if (!(n > 0.5)) return false;
    for (int k = 0; k < 3; ++k) pos[k] = v.pos[k];
    fwd[0] = x / n, fwd[1] = y / n, fwd[2] = z / n;
    return true;
}

}  // namespace st::nametags

namespace st::game {

// action name_tags <op> [arg]: 0 normal, 1 self test, 2 log the camera, 3 <0/1> off / on.
bool NameTagCommand(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept {
    using namespace st::nametags;
    auto say = [&](const char* text) {
        if (msg && cap) std::snprintf(msg, cap, "%s", text);
    };
    switch (op) {
        case 0:
            g_self_test = false;
            say("name tags: normal (the partner's tag)");
            return true;
        case 1:
            g_self_test = true;
            say("name tags: self test (a tag above your own Sam)");
            return true;
        case 3:
            g_on = v[1] != 0 && g_cfg.name_tags;
            say(g_on ? "name tags on" : "name tags off");
            Log("[tags] %s by debug action", g_on ? "on" : "off");
            return true;
        case 2: break;
        default: say("name tags: unknown op (0 normal, 1 self test, 2 log camera, 3 off/on)"); return false;
    }
    game::Entity* local = nullptr;
    game::TryGetLocalPlayer(&local);
    if (!local || !Resolve()) {
        say("name tags: no local player or byte checks failed");
        return false;
    }
    const std::uintptr_t view = ViewOf(local);
    nametag::View cv;
    if (!view || !ReadView(view, &cv)) {
        say("name tags: camera view unreadable");
        return false;
    }
    const float* a0 = cv.axis[0];
    const float* a1 = cv.axis[1];
    const float* a2 = cv.axis[2];
    const double cx = double(a0[1]) * a1[2] - double(a0[2]) * a1[1], cy = double(a0[2]) * a1[0] - double(a0[0]) * a1[2],
                 cz = double(a0[0]) * a1[1] - double(a0[1]) * a1[0];
    const double handed = cx * a2[0] + cy * a2[1] + cz * a2[2];
    const double fov_v = 2.0 * std::atan(cv.top / cv.near_plane) * 57.29577951308232;
    Log("[tags] camera: pos (%.2f, %.2f, %.2f) a0 (%.3f, %.3f, %.3f) a1 (%.3f, %.3f, %.3f) a2 (%.3f, %.3f, %.3f) "
        "dot(cross(a0,a1),a2) %.3f near %.3f far %.1f top %.4f right %.4f aspect %.4f fov_v %.1f deg",
        cv.pos[0], cv.pos[1], cv.pos[2], a0[0], a0[1], a0[2], a1[0], a1[1], a1[2], a2[0], a2[1], a2[2], handed, cv.near_plane,
        cv.far_plane, cv.top, cv.right, cv.right / cv.top, fov_v);
    double head[3];
    if (ReadPos(local, head)) {
        const overlay::Stats st = overlay::GetStats();
        const float w = st.width ? static_cast<float>(st.width) : 1920.0f, h = st.height ? static_cast<float>(st.height) : 1080.0f;
        const double feet[3] = {head[0], head[1], head[2]};
        head[2] += kTagHeightM;
        const nametag::Projected pf = nametag::Project(cv, feet, w, h), ph = nametag::Project(cv, head, w, h);
        Log("[tags] own Sam on a %.0fx%.0f buffer: feet (%.0f, %.0f)%s, tag anchor (%.0f, %.0f)%s, %.2f m from the camera", w, h, pf.x,
            pf.y, pf.visible ? "" : " off-screen", ph.x, ph.y, ph.visible ? "" : " off-screen", ph.dist);
    }
    say("name tags: camera logged");
    return true;
}

}  // namespace st::game
