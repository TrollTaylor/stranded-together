#include "partner_outfit.hpp"

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "body_table.hpp"
#include "log.hpp"
#include "partner_source.hpp"
#include "room_outfit_rules.hpp"

namespace st::outfit {
namespace {

namespace mem = partner::mem;
using mem::Q;
using mem::Rd;

// ---------------------------------------------------------------- ds.exe (RVAs at base 0x140000000)
// Player init 0x14271FA60 caches its components: inventory at +0x54C8, streaming control at +0x54F8.
constexpr std::uintptr_t kInitInvStore = 0x271FC22;     // mov [rbx+0x54C8],rax
constexpr std::uintptr_t kInitScStore = 0x271FC90;      // mov [rbx+0x54F8],rax
// Inventory restore 0x1427E70C0: inv +0x4BDC..+0x4BE1 = game state +0x5608..+0x560D.
constexpr std::uintptr_t kRestoreOutfit = 0x27E710B;    // movzx eax,[r13+0x5608]; mov [rcx+0x4BDC],al
// 0x1427E7850 (inv, entity): colours an entity with inv +0x4BE1 (the AKukan menu Sam).
constexpr std::uintptr_t kColourFromInv = 0x27E7850;    // push rsi; sub rsp,40; movzx esi,[rcx+0x4BE1]
// 0x142686810 (mesh controller, idx): the exos variation; index byte at controller +0x3A50.
constexpr std::uintptr_t kSetExos = 0x2686810;
constexpr std::uintptr_t kSetExosCall = 0x27F3153;      // the inventory update calls it
// 0x1427A2EB0 (streaming control, idx): variant (+0x78 ref) -> ArtPartsVariations[idx].
constexpr std::uintptr_t kVariantApv = 0x27A2EB0;
// Called by us (game thread):
constexpr std::uintptr_t kApplyApv = 0x22BC100;    // ApplyArtPartsVariation(entity, ArtPartsVariationResource*)
constexpr std::uintptr_t kRemoveComp = 0x235B910;  // remove a component from its entity (comp +0x48)
constexpr std::uintptr_t kSuitColour = 0x284CDA0;  // (entity, u8 colour): shader variable 0x2967E719 = colour (< 15)
constexpr std::uintptr_t kPreviewColourCall = 0x272EAC9;  // the uniform menu's preview calls it
constexpr std::uintptr_t kColourHash = 0x4BE7BA4;  // 19 E7 67 29
constexpr std::uintptr_t kColourLimit = 0x3D60906;  // 0x0F
constexpr std::uintptr_t kPlayerVtable = 0x3D7D0D8;

constexpr std::uintptr_t kPInv = 0x54C8, kPSc = 0x54F8, kPExos = 0xA60 + 0x3A50;  // 0x44B0
constexpr std::uintptr_t kInvOutfit = 0x4BDC;  // gA lens, gB lens, gA frame, gB frame, cap colour, suit colour
constexpr std::uintptr_t kCompOwner = 0x48;
constexpr std::uintptr_t kScVariantIdx = 0x74, kScVariantRef = 0x78;
constexpr std::uintptr_t kVarCount = 0x48, kVarData = 0x50;  // DSPlayerBodyVariant ArtPartsVariations
constexpr std::uintptr_t kObjGuid = 0x08, kObjRefs = 0x18;   // RTTIRefObject ObjectUUID, refcount
constexpr std::uintptr_t kApvCompRes = 0x30, kApvCompApplied = 0x50;
constexpr std::uintptr_t kEntComponents = 0x90;  // i32 count, data at +0x98
constexpr std::uintptr_t kEntArtPartsData = 0x180;
constexpr int kExosCount = wire::kExosCount;
constexpr double kExosSettleMs = 500;  // a wanted exos index must hold this long before the body changes

// ds/entities/player: the porter-suit DSPlayerBodyVariant; ds/entities/cinematics/demo_sam_clean's
// own variation apv_sam_body_suiti_v01 (game data, tools/re/ds_archive.py).
constexpr const char* kPorterVariantGuid = "80279532-dbc2-d745-a04b-14d90ec09fe3";
constexpr const char* kSamCleanV01Guid = "ad5fea32-5260-2649-8305-825ba6253234";

struct State {
    bool checked = false, ok = false, off = false;
    char why[160] = "not checked";
    std::uint8_t porter[16] = {}, v01[16] = {};
} g;

bool Check(const char* name, std::uintptr_t rva, const char* sig) {
    if (mem::CodeMatches(mem::Base() + rva, sig)) return true;
    std::snprintf(g.why, sizeof(g.why), "%s at ds.exe+0x%llX does not match", name, static_cast<unsigned long long>(rva));
    return false;
}

const char* TypeOf(std::uintptr_t obj) {
    const char* n = obj ? mem::RttiName(mem::RttiOf(obj)) : nullptr;
    return n ? n : "?";
}
bool IsType(std::uintptr_t obj, const char* name) { return obj && std::strcmp(TypeOf(obj), name) == 0; }

void GuidText(const std::uint8_t* g16, char* out, std::size_t cap) {
    std::snprintf(out, cap, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", g16[3], g16[2], g16[1], g16[0],
                  g16[5], g16[4], g16[7], g16[6], g16[8], g16[9], g16[10], g16[11], g16[12], g16[13], g16[14], g16[15]);
}

// The player's components, checked: inventory and streaming control, each owned by the player.
bool PlayerParts(std::uintptr_t p, std::uintptr_t* inv, std::uintptr_t* sc) {
    if (!p || Q(p) != mem::Base() + kPlayerVtable) return false;
    *inv = Q(p + kPInv);
    *sc = Q(p + kPSc);
    if (!*inv || Q(*inv + kCompOwner) != p || !IsType(*inv, "DSPlayerInventoryComponent")) return false;
    if (!*sc || Q(*sc + kCompOwner) != p || !IsType(*sc, "DSPlayerStreamingControlComponent")) return false;
    return true;
}

// The player's current body variant when it is loaded (streaming ref at sc +0x78, read the way
// 0x141DDE990 does: box = [ref]; loaded when (i8)(box[1] >> 52) < 0; object = [box[0] + 0x20]).
std::uintptr_t CurrentVariant(std::uintptr_t sc) {
    const std::uintptr_t box = Q(sc + kScVariantRef);
    std::uintptr_t blk[2] = {};
    if (!box || !mem::Read(box, blk, sizeof(blk)) || !blk[0]) return 0;
    if (static_cast<std::int8_t>(static_cast<std::uint8_t>(blk[1] >> 52)) >= 0) return 0;
    const std::uintptr_t v = Q(blk[0] + 0x20);
    return IsType(v, "DSPlayerBodyVariant") ? v : 0;
}

// ArtPartsVariations[idx] of our own player's variant, only when that variant is the porter suit
// with its 44 entries (the data the remote player's index refers to). `variant_out` for the log.
std::uintptr_t ExosResource(std::uintptr_t local, int idx, std::uintptr_t* variant_out, const char** why) {
    *variant_out = 0;
    std::uintptr_t inv = 0, sc = 0;
    if (!PlayerParts(local, &inv, &sc)) {
        *why = "our player does not check out";
        return 0;
    }
    const std::uintptr_t v = CurrentVariant(sc);
    *variant_out = v;
    std::uint8_t guid[16] = {};
    if (!v || !mem::Read(v + kObjGuid, guid, 16)) {
        *why = "our player's body variant is not loaded";
        return 0;
    }
    if (std::memcmp(guid, g.porter, 16) != 0) {
        *why = "our player does not wear the porter suit (the variations come from it)";
        return 0;
    }
    std::int32_t n = 0;
    if (!Rd(v + kVarCount, n) || n != kExosCount || idx < 0 || idx >= n) {
        *why = "the porter suit's variation list is not the expected 44 entries";
        return 0;
    }
    const std::uintptr_t r = Q(Q(v + kVarData) + 8ull * static_cast<unsigned>(idx));
    if (!IsType(r, "ArtPartsVariationResource")) {
        *why = "the variation entry is not an ArtPartsVariationResource";
        return 0;
    }
    return r;
}

// Components of `e` that are art-parts variations (component +0x30 an ArtPartsVariationResource,
// owner +0x48 == e). Returns the count; fills up to `cap`.
unsigned Variations(std::uintptr_t e, std::uintptr_t* comps, std::uintptr_t* res, unsigned cap) {
    std::int32_t n = 0;
    if (!e || !Rd(e + kEntComponents, n) || n <= 0 || n > 512) return 0;
    const std::uintptr_t data = Q(e + kEntComponents + 8);
    unsigned k = 0;
    for (std::int32_t i = 0; i < n; ++i) {
        const std::uintptr_t c = Q(data + 8ull * static_cast<unsigned>(i));
        if (!c || Q(c + kCompOwner) != e) continue;
        const std::uintptr_t r = Q(c + kApvCompRes);
        if (!IsType(r, "ArtPartsVariationResource")) continue;
        if (k < cap) comps[k] = c, res[k] = r;
        ++k;
    }
    return k;
}

std::uintptr_t FindVariation(std::uintptr_t e, std::uintptr_t want_res, const std::uint8_t* want_guid) {
    std::uintptr_t c[16], r[16];
    const unsigned n = Variations(e, c, r, 16);
    for (unsigned i = 0; i < n && i < 16; ++i) {
        if (want_res && r[i] == want_res) return c[i];
        std::uint8_t gd[16];
        if (want_guid && mem::Read(r[i] + kObjGuid, gd, 16) && std::memcmp(gd, want_guid, 16) == 0) return c[i];
    }
    return 0;
}

// ---------------------------------------------------------------- SEH thunks (game thread)
bool CallApply(std::uintptr_t e, std::uintptr_t res) {
    __try {
        reinterpret_cast<void (*)(std::uintptr_t, std::uintptr_t)>(mem::Base() + kApplyApv)(e, res);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool CallRemove(std::uintptr_t comp) {
    __try {
        reinterpret_cast<void (*)(std::uintptr_t)>(mem::Base() + kRemoveComp)(comp);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool CallColour(std::uintptr_t e, std::uint8_t colour) {
    __try {
        reinterpret_cast<void (*)(std::uintptr_t, std::uint8_t)>(mem::Base() + kSuitColour)(e, colour);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void Fault(const char* what) {
    g.off = true;
    Log("[outfit] %s FAULTED: outfit matching off for this session", what);
}

}  // namespace

bool Available() noexcept {
    if (g.checked) return g.ok && !g.off;
    g.checked = true;
    const std::uintptr_t b = mem::Base();
    bool ok = Check("player init inventory cache (+0x54C8)", kInitInvStore, "48 89 83 C8 54 00 00") &&
              Check("player init streaming-control cache (+0x54F8)", kInitScStore, "48 89 83 F8 54 00 00") &&
              Check("inventory outfit restore (+0x4BDC)", kRestoreOutfit, "41 0F B6 85 08 56 00 00 88 81 DC 4B 00 00") &&
              Check("suit colour from inventory (+0x4BE1)", kColourFromInv, "40 56 48 83 EC 40 0F B6 B1 E1 4B 00 00") &&
              Check("set exos variation (+0x3A50)", kSetExos, "48 89 6C 24 18 56 48 83 EC 20 0F B6 EA 48 8B F1 38 91 50 3A 00 00") &&
              Check("set exos call site", kSetExosCall, "E8") &&
              Check("variant variation lookup (+0x78)", kVariantApv, "48 89 5C 24 08 57 48 83 EC 20 48 8D 79 78 48 63 DA") &&
              Check("ApplyArtPartsVariation", kApplyApv, "48 83 EC 28 48 85 C9 74 19 48 85 D2 74 14 41 B0 01 E8") &&
              Check("component removal", kRemoveComp, "48 8B D1 48 8B 49 48 E9") &&
              Check("apply suit colour", kSuitColour, "40 56 48 83 EC 40 3A 15 ?? ?? ?? ?? 0F B6 F2 0F 83") &&
              Check("uniform preview colour call", kPreviewColourCall, "E8");
    if (ok && (mem::RipTarget(b + kSetExosCall, 1, 5) != b + kSetExos || mem::RipTarget(b + kPreviewColourCall, 1, 5) != b + kSuitColour ||
               mem::RipTarget(b + kSuitColour + 6, 2, 6) != b + kColourLimit)) {
        std::snprintf(g.why, sizeof(g.why), "the exos / suit colour call sites do not lead where expected");
        ok = false;
    }
    std::uint32_t hash = 0;
    std::uint8_t limit = 0;
    if (ok && (!Rd(b + kColourHash, hash) || hash != 0x2967E719u || !Rd(b + kColourLimit, limit) || limit != 15)) {
        std::snprintf(g.why, sizeof(g.why), "suit colour hash / limit constants differ");
        ok = false;
    }
    if (ok && (!bodies::ParseGuid(kPorterVariantGuid, g.porter) || !bodies::ParseGuid(kSamCleanV01Guid, g.v01))) {
        std::snprintf(g.why, sizeof(g.why), "internal: bad GUID text");
        ok = false;
    }
    if (ok) std::snprintf(g.why, sizeof(g.why), "ok");
    Log("[outfit] outfit matching %s%s", ok ? "available (inventory +0x4BDC, exos +0x44B0, ApplyArtPartsVariation, suit colour verified)"
                                            : "unavailable: ",
        ok ? "" : g.why);
    g.ok = ok;
    return ok;
}

bool ReadLocal(std::uintptr_t p, wire::Outfit* out) noexcept {
    if (!out || !Available()) return false;
    std::uintptr_t inv = 0, sc = 0;
    if (!PlayerParts(p, &inv, &sc)) return false;
    std::uint8_t b[6] = {};
    std::uint8_t variant = 0xFF, exos = 0xFF;
    if (!mem::Read(inv + kInvOutfit, b, sizeof(b)) || !Rd(sc + kScVariantIdx, variant) || !Rd(p + kPExos, exos)) return false;
    wire::Outfit o;
    o.variant = variant;
    o.exos = exos < kExosCount ? exos : 0xFF;
    // The exos index belongs to the current variant's own list: sent only for the porter suit
    // (checked by GUID when the variant is loaded, else by its index).
    const std::uintptr_t v = CurrentVariant(sc);
    std::uint8_t guid[16] = {};
    const bool porter = v ? mem::Read(v + kObjGuid, guid, 16) && std::memcmp(guid, g.porter, 16) == 0 : variant == wire::kPorterVariant;
    if (!porter) o.exos = 0xFF;
    o.glasses_a_lens = b[0], o.glasses_b_lens = b[1], o.glasses_a_frame = b[2], o.glasses_b_frame = b[3];
    o.cap_colour = b[4];
    o.suit_colour = b[5];
    *out = o;
    return true;
}

bool LocalWearsPorter(std::uintptr_t p) noexcept {
    if (!Available()) return false;
    std::uintptr_t inv = 0, sc = 0;
    std::uint8_t variant = 0xFF;
    if (!PlayerParts(p, &inv, &sc) || !Rd(sc + kScVariantIdx, variant)) return false;
    const std::uintptr_t v = CurrentVariant(sc);
    std::uint8_t guid[16] = {};
    return v ? mem::Read(v + kObjGuid, guid, 16) && std::memcmp(guid, g.porter, 16) == 0 : variant == wire::kPorterVariant;
}

const char* Describe(const wire::Outfit& o, char* buf, std::size_t cap) noexcept {
    std::snprintf(buf, cap, "variant %u exos %u suit colour %u cap %u glasses A lens %u frame %u, B lens %u frame %u", o.variant, o.exos,
                  o.suit_colour, o.cap_colour, o.glasses_a_lens, o.glasses_a_frame, o.glasses_b_lens, o.glasses_b_frame);
    return buf;
}

void Forget(Dress& d) noexcept { d = Dress{}; }

void Apply(Dress& d, std::uintptr_t e, const wire::Outfit& want, std::uintptr_t local, double now_ms) noexcept {
    if (!e || !Available()) return;
    if (d.e != e) {
        Forget(d);
        d.e = e;
    }
    if (now_ms < d.next_ms) return;
    d.next_ms = now_ms + 250;
    // 1. The body's own darker suit texture (apv_sam_body_suiti_v01) goes first.
    if (!d.v01_done) {
        const std::uintptr_t c = FindVariation(e, 0, g.v01);
        d.v01_done = true;
        if (!c) {
            Log("[outfit] body %p: no apv_sam_body_suiti_v01 variation (not demo_sam_clean?)", reinterpret_cast<void*>(e));
            return;
        }
        if (!CallRemove(c)) return Fault("removing the body's own suit variation");
        ++d.applies;
        d.colour = 0xFF;  // a variation change may reset the suit colour: set it again
        Log("[outfit] body %p: removed its own suit variation apv_sam_body_suiti_v01 (component %p): the porter suit's base texture",
            reinterpret_cast<void*>(e), reinterpret_cast<void*>(c));
        return;
    }
    // 2. The exos variation (silver / gold suit, glove and exoskeleton tints). Only the porter
    // suit's index counts (wire::PorterExos), and a change must hold kExosSettleMs first (a
    // sender's first frames, or a moment mid-change, can read 0xFF: no flash of the plain suit).
    const std::uint8_t want_exos = wire::PorterExos(want);
    if (want_exos != d.pending_exos) {
        // the body's first reading of an index (none added yet) needs no wait: there is no tint to flash away from
        const bool first = d.pending_exos == 0xFE && !d.exos_res && want_exos != 0xFF;
        d.pending_exos = want_exos;
        d.pending_ms = first ? now_ms - kExosSettleMs : now_ms;
    }
    if (want_exos != d.exos && now_ms - d.pending_ms >= kExosSettleMs) {
        if (d.exos_res) {
            const std::uintptr_t c = FindVariation(e, d.exos_res, nullptr);
            if (c && !CallRemove(c)) return Fault("removing the exos variation");
            if (c) ++d.applies;
            Log("[outfit] body %p: exos variation %u removed%s", reinterpret_cast<void*>(e), d.exos, c ? "" : " (component already gone)");
            d.exos_res = 0;
            d.exos = 0xFF;
            d.colour = 0xFF;
            return;
        }
        if (want_exos == 0xFF) {
            d.exos = 0xFF;
            return;
        }
        std::uintptr_t variant = 0;
        const char* why = "";
        const std::uintptr_t res = ExosResource(local, want_exos, &variant, &why);
        if (!res) {
            if (d.want_logged_exos != want_exos) {
                d.want_logged_exos = want_exos;
                Log("[outfit] body %p: exos variation %u not applied: %s", reinterpret_cast<void*>(e), want_exos, why);
            }
            d.next_ms = now_ms + 2000;  // retry later (our player may put the suit back on)
            return;
        }
        std::int32_t refs0 = 0, refs1 = 0;
        Rd(res + kObjRefs, refs0);
        if (!CallApply(e, res)) return Fault("ApplyArtPartsVariation");
        ++d.applies;
        Rd(res + kObjRefs, refs1);
        const std::uintptr_t c = FindVariation(e, res, nullptr);
        std::uint8_t applied = 0;
        if (c) Rd(c + kApvCompApplied, applied);
        std::uint8_t gd[16] = {};
        mem::Read(res + kObjGuid, gd, 16);
        char gt[40];
        GuidText(gd, gt, sizeof(gt));
        Log("[outfit] body %p: exos variation %u (resource %p guid %s, from our porter suit %p) added: component %p applied %u; "
            "resource refs %d -> %d",
            reinterpret_cast<void*>(e), want_exos, reinterpret_cast<void*>(res), gt, reinterpret_cast<void*>(variant),
            reinterpret_cast<void*>(c), applied, refs0, refs1);
        d.exos = want_exos;
        d.exos_res = res;
        d.colour = 0xFF;
        return;
    }
    // 3. The suit colour 0..14. Like the game's own inventory update (0x1427F2ED3), 15 / 16 (silver,
    // gold: the exos textures) leave the shader variable as it was, on the player and here alike.
    const std::uint8_t want_colour = want.suit_colour <= 14 ? want.suit_colour : 0xFF;
    if (want_colour != 0xFF && want_colour != d.colour) {
        const std::uintptr_t apd = Q(e + kEntArtPartsData);
        if (!apd || !mem::DerivesFrom(mem::RttiOf(apd), "ArtPartsData") || Q(apd + kCompOwner) != e) {
            if (d.want_logged_colour != want_colour) {
                d.want_logged_colour = want_colour;
                Log("[outfit] body %p: suit colour %u not applied: no ArtPartsData component of its own at +0x180 (%s)",
                    reinterpret_cast<void*>(e), want_colour, TypeOf(apd));
            }
            return;
        }
        if (!CallColour(e, want_colour)) return Fault("the suit colour function");
        ++d.applies;
        ++d.colour_sets;
        Log("[outfit] body %p: suit colour %u set (#%u; was %u)", reinterpret_cast<void*>(e), want_colour, d.colour_sets, d.colour);
        d.colour = want_colour;
    }
}

bool OwnSuitColour(std::uintptr_t p, std::uintptr_t* addr, std::uint8_t* value) noexcept {
    std::uintptr_t inv = 0, sc = 0;
    *addr = 0;
    if (!Available() || !PlayerParts(p, &inv, &sc) || !Rd(inv + kInvOutfit + 5, *value)) return false;
    *addr = inv + kInvOutfit + 5;
    return true;
}

bool TestSetOwnSuitColour(std::uintptr_t p, std::uint8_t colour) noexcept {
    std::uintptr_t inv = 0, sc = 0;
    if (!Available() || colour > 16 || !PlayerParts(p, &inv, &sc)) return false;
    std::uint8_t before = 0, exos = 0;
    if (!Rd(inv + kInvOutfit + 5, before)) return false;
    Rd(p + kPExos, exos);
    SIZE_T n = 0;
    const bool ok = WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(inv + kInvOutfit + 5), &colour, 1, &n) && n == 1;
    Log("[outfit] TEST: our own Sam's suit colour byte (inventory %p +0x4BE1) %u -> %u %s (exos %u now; the game's inventory "
        "update recomputes the exos variation from it)",
        reinterpret_cast<void*>(inv), before, colour, ok ? "written" : "NOT written", exos);
    return ok;
}

// ---------------------------------------------------------------- Sam's clothes change (section 34)
namespace {
// DSPlayerStreamingControlComponent update 0x1427CAAE0 (RTTI 0x144BCF890).
constexpr std::uintptr_t kScStateRead = 0x27CAB2A;    // mov ecx,[rbp+0x70]; sub ecx,1: the switch state
constexpr std::uintptr_t kScRequestRead = 0x27CAB78;  // movzx ecx,[rbp+0x76]; xor r14d,r14d; cmp cl,8: the request (8: the graph)
constexpr std::uintptr_t kScWantedStore = 0x27CABB1;  // mov [rbp+0x75],al; cmp al,[rbp+0x74]: wanted vs current
constexpr std::uintptr_t kScApply = 0x27CAE99;        // movzx r15d,[rbp+0x75]; mov [rbp+0x74],r15b: state 2 applies it
constexpr std::uintptr_t kScLoadingSet = 0x27CAC45;   // lea rcx,"DSPlayerLoadingModel"; call the named-flag setter
// Player init 0x14271FA60 caches the paint effect (DSPlayerPaintEffectComponent) at +0x54F0.
constexpr std::uintptr_t kInitPaintStore = 0x271FCBC;  // mov [rbx+0x54F0],rax
// The paint effect's ready test (state 3 waits on it): +0xAA2C, or +0x38 and +0xAA21 == 3.
constexpr std::uintptr_t kPaintReady = 0x2707090;
// The private room's uniform menu (subspace state 3, 0x1426AD430): sc +0x74 == 1 -> sc +0x76 = 0 (the porter suit).
constexpr std::uintptr_t kWardrobeAsk = 0x26AD9C7;
// The named-flag setter 0x1422AF7B0: its table (32-byte entries: key = crc | 0x80000000, +8 crc, +0x10 value) and size.
constexpr std::uintptr_t kFlagSetter = 0x22AF7B0, kFlagTableLoad = 0x22AF852, kFlagCapLoad = 0x22AF8A7;
constexpr std::uintptr_t kFlagTable = 0x7BA1E98, kFlagCap = 0x7BA1EA4, kLoadingName = 0x3D839B8;

constexpr std::uintptr_t kPPaint = 0x54F0;
constexpr std::uintptr_t kScState = 0x70, kScWanted = 0x75, kScRequest = 0x76, kScDelay = 0x98, kScWait = 0x9C;
constexpr std::uintptr_t kPaintStage = 0xAA21, kPaintTimer = 0xAA28, kPaintReleased = 0xAA2C, kPaintFlags = 0xAA2E, kPaintOn = 0x38;

struct Clothes {
    bool checked = false, ok = false, flag_ok = false;
    char why[160] = "not checked";
    std::uint32_t loading_crc = 0;
} gc;

bool CheckC(const char* name, std::uintptr_t rva, const char* sig) {
    if (mem::CodeMatches(mem::Base() + rva, sig)) return true;
    std::snprintf(gc.why, sizeof(gc.why), "%s at ds.exe+0x%llX does not match", name, static_cast<unsigned long long>(rva));
    return false;
}

// The streaming control of player p, checked (type and owner); 0 when not.
std::uintptr_t StreamingControl(std::uintptr_t p) {
    if (!p || Q(p) != mem::Base() + kPlayerVtable) return 0;
    const std::uintptr_t sc = Q(p + kPSc);
    return sc && Q(sc + kCompOwner) == p && IsType(sc, "DSPlayerStreamingControlComponent") ? sc : 0;
}

int ReadLoadingModel() {
    if (!gc.flag_ok) return -1;
    const std::uintptr_t b = mem::Base();
    const std::uintptr_t table = Q(b + kFlagTable);
    std::int32_t cap = 0;
    if (!table || !Rd(b + kFlagCap, cap) || cap <= 0 || cap > 4096 || (cap & (cap - 1)) != 0) return -1;
    const std::uint32_t key = gc.loading_crc | 0x80000000u;
    // linear probing from the home slot, as the setter does (a torn read gives -1 or a stale value: diagnostics only)
    for (std::int32_t i = 0, s = static_cast<std::int32_t>(key & static_cast<std::uint32_t>(cap - 1)); i < cap; ++i, s = (s + 1) & (cap - 1)) {
        std::uint32_t e[6] = {};
        if (!mem::Read(table + 32ull * static_cast<unsigned>(s), e, sizeof(e))) return -1;
        if (e[0] == 0) return -1;
        if (e[0] == key && e[2] == gc.loading_crc) return static_cast<int>(e[4] & 0xFF);
    }
    return -1;
}
}  // namespace

bool ClothesAvailable() noexcept {
    if (gc.checked) return gc.ok;
    gc.checked = true;
    const std::uintptr_t b = mem::Base();
    bool ok = CheckC("player init streaming-control cache (+0x54F8)", kInitScStore, "48 89 83 F8 54 00 00") &&
              CheckC("player init paint-effect cache (+0x54F0)", kInitPaintStore, "48 89 83 F0 54 00 00") &&
              CheckC("streaming control state read (+0x70)", kScStateRead, "8B 4D 70 83 E9 01") &&
              CheckC("streaming control request read (+0x76, 8)", kScRequestRead, "0F B6 4D 76 45 33 F6 80 F9 08") &&
              CheckC("streaming control wanted store (+0x75 vs +0x74)", kScWantedStore, "88 45 75 3A 45 74") &&
              CheckC("streaming control apply (+0x74 = +0x75)", kScApply, "44 0F B6 7D 75 44 88 7D 74") &&
              CheckC("paint effect ready test", kPaintReady,
                     "80 B9 2C AA 00 00 00 75 12 80 79 38 00 74 09 80 B9 21 AA 00 00 03 74 03 32 C0 C3 B0 01 C3") &&
              CheckC("uniform menu's porter-suit request", kWardrobeAsk, "80 78 74 01 75 15 ?? ?? ?? ?? ?? ?? ?? ?? 44 88 78 76");
    if (ok) std::snprintf(gc.why, sizeof(gc.why), "ok");
    gc.ok = ok;
    // The DSPlayerLoadingModel flag is diagnostics only: its own checks.
    char name[24] = {};
    gc.flag_ok = ok && CheckC("DSPlayerLoadingModel set", kScLoadingSet, "48 8D 0D ?? ?? ?? ?? E8") &&
                 mem::RipTarget(b + kScLoadingSet, 3, 7) == b + kLoadingName && mem::RipTarget(b + kScLoadingSet + 7, 1, 5) == b + kFlagSetter &&
                 mem::Read(b + kLoadingName, name, 21) && std::strcmp(name, "DSPlayerLoadingModel") == 0 &&
                 CheckC("named-flag table load", kFlagTableLoad, "4C 8B 15 ?? ?? ?? ?? 4D 85 D2") &&
                 mem::RipTarget(b + kFlagTableLoad, 3, 7) == b + kFlagTable &&
                 CheckC("named-flag size load", kFlagCapLoad, "44 8B 35 ?? ?? ?? ?? 45 8D 5E FF 8B EB 81 CD 00 00 00 80 8B FD 41 23 FB 48 63 D7 44 8B CF 48 C1 E2 05") &&
                 mem::RipTarget(b + kFlagCapLoad, 3, 7) == b + kFlagCap;
    if (ok && !gc.flag_ok) std::snprintf(gc.why, sizeof(gc.why), "ok (DSPlayerLoadingModel not read: its sites differ)");
    gc.loading_crc = roomoutfit::Crc32c("DSPlayerLoadingModel");
    Log("[clothes] Sam's clothes change %s%s", ok ? "readable (streaming control +0x70/+0x74/+0x75/+0x76, paint effect +0x54F0 ready test verified)"
                                                   : "unreadable: the clothes rule and the room outfit's variant rule are off: ",
        ok ? (gc.flag_ok ? "; DSPlayerLoadingModel readable" : "; DSPlayerLoadingModel not readable") : gc.why);
    return ok;
}

const char* ClothesWhy() noexcept { return gc.why; }

bool ReadVariantState(std::uintptr_t p, VariantState* out, bool paint) noexcept {
    if (!out || !ClothesAvailable()) return false;
    const std::uintptr_t sc = StreamingControl(p);
    if (!sc) return false;
    VariantState v;
    std::uint8_t b[3] = {};
    if (!Rd(sc + kScState, v.state) || !mem::Read(sc + kScVariantIdx, b, sizeof(b))) return false;
    static_assert(kScWanted == kScVariantIdx + 1 && kScRequest == kScVariantIdx + 2, "variant bytes");
    v.cur = b[0], v.wanted = b[1], v.request = b[2];
    Rd(sc + kScDelay, v.delay);
    Rd(sc + kScWait, v.wait);
    if (paint) {
        const std::uintptr_t pe = Q(p + kPPaint);
        std::uint8_t on = 0;
        if (pe && Q(pe + kCompOwner) == p && IsType(pe, "DSPlayerPaintEffectComponent") && Rd(pe + kPaintStage, v.paint_stage) &&
            Rd(pe + kPaintTimer, v.paint_timer) && Rd(pe + kPaintReleased, v.paint_released) && Rd(pe + kPaintFlags, v.paint_flags) &&
            Rd(pe + kPaintOn, on)) {
            v.paint_ok = true;
            v.paint_ready = v.paint_released != 0 || (on != 0 && v.paint_stage == 3);
        }
        v.loading_model = ReadLoadingModel();
    }
    *out = v;
    return true;
}

const char* DescribeVariantState(const VariantState& v, char* buf, std::size_t cap) noexcept {
    char paint[128] = "paint unread";
    if (v.paint_ok)
        std::snprintf(paint, sizeof(paint), "paint %s (stage %u, timer %.1f s, released %u, flags 0x%02X)",
                      v.paint_ready ? (v.paint_timer >= 59.9f ? "released by the game's 60 s timeout" : "ready") : "not ready", v.paint_stage,
                      static_cast<double>(v.paint_timer), v.paint_released, v.paint_flags);
    char lm[8] = "?";
    if (v.loading_model >= 0) std::snprintf(lm, sizeof(lm), "%d", v.loading_model);
    std::snprintf(buf, cap, "variant %u -> %u (request %u, state %d, delay %.1f, wait %.1f s); %s; DSPlayerLoadingModel %s", v.cur, v.wanted, v.request,
                  v.state, static_cast<double>(v.delay), static_cast<double>(v.wait), paint, lm);
    return buf;
}

bool TestRequestVariant(std::uintptr_t p, std::uint8_t request, char* msg, std::size_t cap) noexcept {
    if (request > roomoutfit::kVariantGraph) {
        std::snprintf(msg, cap, "request 0..7 (a variant) or 8 (the game's own choice)");
        return false;
    }
    if (!ClothesAvailable()) {
        std::snprintf(msg, cap, "unavailable: %s", gc.why);
        return false;
    }
    const std::uintptr_t sc = StreamingControl(p);
    std::uint8_t before = 0xFF;
    if (!sc || !Rd(sc + kScRequest, before)) {
        std::snprintf(msg, cap, "our player's streaming control does not check out");
        return false;
    }
    SIZE_T n = 0;
    const bool ok = WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(sc + kScRequest), &request, 1, &n) && n == 1;
    VariantState v;
    char d[200] = "";
    if (ReadVariantState(p, &v, true)) DescribeVariantState(v, d, sizeof(d));
    std::snprintf(msg, cap, "TEST: Sam's body variant request (streaming control %p +0x76) %u -> %u %s; %s", reinterpret_cast<void*>(sc), before, request,
                  ok ? "written" : "NOT written", d);
    Log("[clothes] %s", msg);
    return ok;
}

void LogLocal(std::uintptr_t p) noexcept {
    if (!Available()) {
        Log("[outfit] local: unavailable (%s)", g.why);
        return;
    }
    std::uintptr_t inv = 0, sc = 0;
    if (!PlayerParts(p, &inv, &sc)) {
        Log("[outfit] local: player %p does not check out (inventory %p %s, streaming control %p %s)", reinterpret_cast<void*>(p),
            reinterpret_cast<void*>(Q(p + kPInv)), TypeOf(Q(p + kPInv)), reinterpret_cast<void*>(Q(p + kPSc)), TypeOf(Q(p + kPSc)));
        return;
    }
    wire::Outfit o;
    char d[160];
    ReadLocal(p, &o);
    std::uint8_t raw_exos = 0, gs[6] = {};
    Rd(p + kPExos, raw_exos);
    const std::uintptr_t gsp = Q(mem::Base() + 0x7BE7980);
    if (gsp) mem::Read(gsp + 0x5608, gs, sizeof(gs));
    const std::uintptr_t v = CurrentVariant(sc);
    std::uint8_t vg[16] = {};
    std::int32_t n = -1;
    if (v) mem::Read(v + kObjGuid, vg, 16), Rd(v + kVarCount, n);
    char vt[40];
    GuidText(vg, vt, sizeof(vt));
    Log("[outfit] local player %p: %s (raw exos byte %u; game state +0x5608: %u %u %u %u %u %u)", reinterpret_cast<void*>(p),
        Describe(o, d, sizeof(d)), raw_exos, gs[0], gs[1], gs[2], gs[3], gs[4], gs[5]);
    Log("[outfit] local player: inventory %p, streaming control %p, current variant %p guid %s%s, %d variations", reinterpret_cast<void*>(inv),
        reinterpret_cast<void*>(sc), reinterpret_cast<void*>(v), vt, std::memcmp(vg, g.porter, 16) == 0 ? " (the porter suit)" : "", n);
    LogBody(p, "local player");
}

void LogBody(std::uintptr_t e, const char* what) noexcept {
    std::uintptr_t c[16], r[16];
    const unsigned n = Variations(e, c, r, 16);
    Log("[outfit] %s %p: %u art-parts variation component(s)%s", what, reinterpret_cast<void*>(e), n, n ? ":" : "");
    for (unsigned i = 0; i < n && i < 16; ++i) {
        std::uint8_t gd[16] = {}, applied = 0;
        std::int32_t refs = 0;
        mem::Read(r[i] + kObjGuid, gd, 16);
        Rd(c[i] + kApvCompApplied, applied);
        Rd(r[i] + kObjRefs, refs);
        char gt[40];
        GuidText(gd, gt, sizeof(gt));
        Log("[outfit]   component %p (%s) resource %p guid %s applied %u refs %d%s", reinterpret_cast<void*>(c[i]), TypeOf(c[i]),
            reinterpret_cast<void*>(r[i]), gt, applied, refs, std::memcmp(gd, g.v01, 16) == 0 ? " = apv_sam_body_suiti_v01" : "");
    }
    const std::uintptr_t apd = Q(e + kEntArtPartsData);
    Log("[outfit]   +0x180 %p (%s), shader variables %p", reinterpret_cast<void*>(apd), TypeOf(apd),
        reinterpret_cast<void*>(apd ? Q(apd + 0xF0) : 0));
}

}  // namespace st::outfit
