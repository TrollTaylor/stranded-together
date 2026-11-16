#include "game_api.hpp"

#include <windows.h>

#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "log.hpp"

namespace st::game {
namespace {

Api g_api;

struct Section {
    std::uintptr_t begin = 0, end = 0;
};

Section TextSection() {
    auto base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (std::memcmp(sec->Name, ".text", 5) == 0) {
            return {reinterpret_cast<std::uintptr_t>(base) + sec->VirtualAddress,
                    reinterpret_cast<std::uintptr_t>(base) + sec->VirtualAddress + sec->Misc.VirtualSize};
        }
    }
    return {};
}

// "48 8B ?? 05" -> bytes + mask
struct Pattern {
    std::vector<std::uint8_t> bytes;
    std::vector<bool> mask;  // true = must match
};

Pattern Parse(const char* sig) {
    Pattern p;
    for (const char* s = sig; *s;) {
        if (*s == ' ') { ++s; continue; }
        if (s[0] == '?') {
            p.bytes.push_back(0);
            p.mask.push_back(false);
            s += (s[1] == '?') ? 2 : 1;
            continue;
        }
        p.bytes.push_back(static_cast<std::uint8_t>(std::strtoul(std::string(s, 2).c_str(), nullptr, 16)));
        p.mask.push_back(true);
        s += 2;
    }
    return p;
}

bool MatchAt(std::uintptr_t at, const Pattern& p) {
    auto b = reinterpret_cast<const std::uint8_t*>(at);
    for (size_t i = 0; i < p.bytes.size(); ++i)
        if (p.mask[i] && b[i] != p.bytes[i]) return false;
    return true;
}

// Returns the unique match, or 0 if none / ambiguous.
std::uintptr_t ScanUnique(const Section& text, const Pattern& p) {
    std::uintptr_t found = 0;
    const std::uintptr_t last = text.end - p.bytes.size();
    const std::uint8_t first = p.bytes[0];
    for (std::uintptr_t at = text.begin; at <= last; ++at) {
        if (*reinterpret_cast<const std::uint8_t*>(at) != first) continue;
        if (!MatchAt(at, p)) continue;
        if (found) return 0;  // ambiguous
        found = at;
    }
    return found;
}

struct Target {
    const char* name;
    std::uintptr_t rva;
    const char* sig;  // unique in ds.exe DC .text (tools/re/mksigs.py)
    void** slot;
};

}  // namespace

std::uintptr_t ModuleBase() { return reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)); }

bool Resolve() {
    if (g_api.ready) return true;
    const Section text = TextSection();
    const std::uintptr_t base = ModuleBase();
    Target targets[] = {
        {"GetLocalDSPlayerEntity", 0x2757A30, "48 83 EC 28 48 8B 0D ?? ?? ?? ?? 48 85 C9 75 07",
         reinterpret_cast<void**>(&g_api.GetLocalPlayerEntity)},
        {"Entity::GetWorldTransform", 0x2362C30,
         "40 53 48 83 EC 60 48 8B C2 48 8B D9 48 8B C8 48 8D 54 24 20 E8 ?? ?? ?? ?? C5 F8 10 00 C5 F8 11 03",
         reinterpret_cast<void**>(&g_api.EntityGetWorldTransform)},
        {"Entity::SetWorldTransform", 0x2361EA0, "48 85 C9 74 1C 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ??",
         reinterpret_cast<void**>(&g_api.EntitySetWorldTransform)},
        {"Entity::ResolveResource", 0x1DDE990, "48 89 5C 24 20 57 48 83 EC 20 48 8B 01 33 C9 89 4C 24 30",
         reinterpret_cast<void**>(&g_api.ResolveEntityResource)},
        {"EntityResource::CreateEntity", 0x23806A0,
         "48 83 EC 58 48 8B 84 24 98 00 00 00 4C 8B 94 24 90 00 00 00",
         reinterpret_cast<void**>(&g_api.EntityResourceCreateEntity)},
        {"Entity::Remove", 0x234CAA0, "48 85 C9 0F 84 ?? ?? ?? ?? 56 48 83 EC 40 48 8B F1",
         reinterpret_cast<void**>(&g_api.EntityRemove)},
        {"SteamRunCallbacksWrapper", 0x1A1F690,
         "48 83 EC 28 48 8B 0D ?? ?? ?? ?? 48 85 C9 74 05 E8 ?? ?? ?? ?? FF 15 ?? ?? ?? ??",
         reinterpret_cast<void**>(&g_api.SteamRunCallbacksWrapper)},
    };
    bool all = true;
    if (!text.begin || text.end <= text.begin) {
        Log("[game] ds.exe has no .text section?!");
        return false;
    }
    for (auto& t : targets) {
        const Pattern p = Parse(t.sig);
        std::uintptr_t at = 0;
        const char* how = "missing";
        const std::uintptr_t known = base + t.rva;
        if (known + p.bytes.size() <= text.end && known >= text.begin && MatchAt(known, p)) {
            at = known;
            how = "known rva";
        } else if ((at = ScanUnique(text, p)) != 0) {
            how = "signature scan";
        }
        *t.slot = reinterpret_cast<void*>(at);
        if (at) {
            Log("[game] %-30s at ds.exe+0x%llX (%s)", t.name, static_cast<unsigned long long>(at - base), how);
        } else {
            Log("[game] %-30s NOT FOUND - unsupported ds.exe build?", t.name);
            all = false;
        }
    }

    // DSPlayerEntity::Sleep override (vtable +0xA0): decode the DSPlayerSystem singleton from it.
    {
        const Pattern p = Parse("48 8B C1 48 8B 0D ?? ?? ?? ?? F6 81 D1 06 02 00 04 74 08 48 8B C8 E9");
        std::uintptr_t at = base + 0x2743810;
        const char* how = "known rva";
        if (!(at + p.bytes.size() <= text.end && MatchAt(at, p))) {
            at = ScanUnique(text, p);
            how = "signature scan";
        }
        if (at) {
            const auto rel_global = *reinterpret_cast<const std::int32_t*>(at + 6);
            g_api.DSPlayerSystem = reinterpret_cast<std::uint8_t**>(at + 10 + rel_global);
            const auto rel_jmp = *reinterpret_cast<const std::int32_t*>(at + 0x17);
            g_api.HumanoidSleep = reinterpret_cast<void*>(at + 0x1B + rel_jmp);
            Log("[game] %-30s at ds.exe+0x%llX (%s): DSPlayerSystem=ds.exe+0x%llX Humanoid::Sleep=ds.exe+0x%llX",
                "DSPlayerEntity::Sleep", static_cast<unsigned long long>(at - base), how,
                static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(g_api.DSPlayerSystem) - base),
                static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(g_api.HumanoidSleep) - base));
        } else {
            Log("[game] %-30s NOT FOUND - unsupported ds.exe build?", "DSPlayerEntity::Sleep");
            all = false;
        }
    }
    g_api.ready = all;
    return all;
}

const Api& Get() { return g_api; }

// ---- SEH-guarded calls. Each helper is a tiny function with no C++ objects
// that need unwinding, so __try is legal here.

bool TryGetLocalPlayer(Entity** out) {
    *out = nullptr;
    if (!g_api.GetLocalPlayerEntity) return false;
    __try {
        *out = g_api.GetLocalPlayerEntity();
        return *out != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool TryGetTransform(Entity* e, WorldTransform* out) {
    if (!e || !g_api.EntityGetWorldTransform) return false;
    __try {
        g_api.EntityGetWorldTransform(out, e);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool TrySetTransform(Entity* e, const WorldTransform& wt) {
    if (!e || !g_api.EntitySetWorldTransform) return false;
    __try {
        g_api.EntitySetWorldTransform(e, &wt);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool TryGetResource(Entity* e, EntityResource** out) {
    *out = nullptr;
    if (!e || !g_api.ResolveEntityResource) return false;
    __try {
        *out = g_api.ResolveEntityResource(static_cast<std::uint8_t*>(e) + entity_off::kResourceRef);
        return *out != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool TryCreateEntity(EntityResource* res, const WorldTransform& wt, const char* name, Entity** out) {
    *out = nullptr;
    if (!res || !g_api.EntityResourceCreateEntity) return false;
    float rot[3][4] = {};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) rot[r][c] = wt.rot.m[r][c];
    CreateEntityPostAction post{};
    __try {
        *out = g_api.EntityResourceCreateEntity(res, &wt.pos, rot, name, false, nullptr, nullptr, &post);
        return true;  // no fault; the caller checks *out
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

namespace {

// The DSPlayerSystem gate byte (bit 4). Set it around calls that may Sleep/Wake
// the partner clone, exactly as the player system does around its own calls
// on Sam (0x1426E0410). Returns null when the player system does not exist yet.
std::uint8_t* GateByte() {
    if (!g_api.DSPlayerSystem) return nullptr;
    __try {
        std::uint8_t* sys = *g_api.DSPlayerSystem;
        return sys ? sys + kDSPlayerSystemGateOffset : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool GateEnter(std::uint8_t* gate, std::uint8_t* saved) {
    __try {
        *saved = *gate;
        *gate = static_cast<std::uint8_t>(*saved | kDSPlayerSystemGateBit);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void GateLeave(std::uint8_t* gate, std::uint8_t saved) {
    __try {
        // Restore only our bit: the callee may legitimately have changed others.
        *gate = static_cast<std::uint8_t>((*gate & ~kDSPlayerSystemGateBit) | (saved & kDSPlayerSystemGateBit));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool Asleep(Entity* e, bool* out) {
    __try {
        *out = (*reinterpret_cast<volatile std::uint64_t*>(static_cast<std::uint8_t*>(e) + entity_off::kFlags) &
                kEntityFlagSleeping) != 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallWake(Entity* e) {
    __try {
        auto vt = *reinterpret_cast<std::uintptr_t*>(e);
        (*reinterpret_cast<void (**)(Entity*)>(vt + kEntityVtWake))(e);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallSleep(Entity* e) {
    __try {
        auto vt = *reinterpret_cast<std::uintptr_t*>(e);
        // arg true: the full sleep (also disables what Wake re-enables, 0x14235F640).
        (*reinterpret_cast<void (**)(Entity*, bool)>(vt + kEntityVtSleep))(e, true);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallRemove(Entity* e) {
    __try {
        g_api.EntityRemove(e, true);  // immediate: finished before the gate is restored
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

bool TryCreatePartner(EntityResource* res, const WorldTransform& wt, const char* name, Entity** out) {
    *out = nullptr;
    std::uint8_t* gate = GateByte();
    std::uint8_t saved = 0;
    if (!gate || !GateEnter(gate, &saved)) return false;
    const bool ok = TryCreateEntity(res, wt, name, out);
    GateLeave(gate, saved);
    return ok;
}

bool TryWakePartner(Entity* e) {
    if (!e) return false;
    bool asleep = false;
    if (!Asleep(e, &asleep)) return false;
    if (!asleep) return true;
    std::uint8_t* gate = GateByte();
    std::uint8_t saved = 0;
    if (!gate || !GateEnter(gate, &saved)) return false;
    const bool ok = CallWake(e);
    GateLeave(gate, saved);
    return ok;
}

bool TryRemovePartner(Entity* e) {
    if (!e || !g_api.EntityRemove) return false;
    std::uint8_t* gate = GateByte();
    std::uint8_t saved = 0;
    if (!gate || !GateEnter(gate, &saved)) return false;
    bool asleep = true;
    bool ok = Asleep(e, &asleep);
    if (ok && !asleep) ok = CallSleep(e);  // sleep it here, gated, so removal never sleeps it ungated
    if (ok) ok = CallRemove(e);
    GateLeave(gate, saved);
    return ok;
}

bool TryReadFlags(Entity* e, std::uint64_t* out) {
    if (!e) return false;
    __try {
        *out = *reinterpret_cast<volatile std::uint64_t*>(static_cast<std::uint8_t*>(e) + entity_off::kFlags);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool TryReadGuid(Entity* e, Guid* out) {
    if (!e) return false;
    __try {
        const auto* p = reinterpret_cast<const std::uint64_t*>(static_cast<std::uint8_t*>(e) + kEntityGuid);
        out->lo = p[0];
        out->hi = p[1];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool TryRemoveEntity(Entity* e) {
    if (!e || !g_api.EntityRemove) return false;
    __try {
        g_api.EntityRemove(e, false);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace st::game
