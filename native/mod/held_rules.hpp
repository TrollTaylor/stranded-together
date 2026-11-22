// What a player holds in a hand, on the other screen (docs/real-partner-body.md section 31), the pure part: which weapon
// resources the sender sends and the receiver draws as a "held clone", and which of the weapon resource's component
// resources the clone keeps. No game memory, no Windows: unit-tested in tests/held_rules_tests.cpp.
//
// Background. Every item in a hand is a DSWeaponEntity (ladder, PCC, climbing anchor, container repair spray, rifles,
// bola gun, grenades ...). v0.9-v0.23 drew only the eight weapons that have a cutscene prop of the same model (kWeaponMap in
// partner_gear.cpp); everything else was dropped at the sender ("no prop for it on the other screen (not sent)"). A copy of
// the game's DSWeaponEntity crashes (its update reads a weapon instance a copy does not have), so the receiver builds a plain
// Entity from the weapon's own resource, re-typed as an EntityResource (held_items.cpp): the real model, real colours.
//
// Modes (session.ini held_items / `gear 26 <mode>`):
//   0 off: v0.23 (only the cutscene-prop weapons);
//   1 tools: the ladder, the PCC and the climbing anchor as held clones;
//   2 all (default): every weapon resource under ds/entities/weapons/ that has no cutscene prop.
#pragma once
#include <cstdint>
#include <cstring>

namespace st::heldrules {

enum class Mode : std::uint8_t { off = 0, tools = 1, all = 2 };

inline bool StartsWith(const char* s, const char* p) { return s && p && std::strncmp(s, p, std::strlen(p)) == 0; }

// The construction tools the user named (ladder, PCC = "builder", climbing anchor = "pile"; every level of the anchor).
inline bool IsToolPath(const char* path) {
    if (!StartsWith(path, "ds/entities/weapons/")) return false;
    const char* n = path + 20;
    return std::strcmp(n, "ladder") == 0 || std::strcmp(n, "builder") == 0 || StartsWith(n, "pile");
}

// Whether a weapon resource without a cutscene prop (`mapped` false) is sent / drawn as a held clone in this mode. A weapon
// that has a cutscene prop (`mapped` true) keeps it (proven since v0.9; the clone is the fallback only when asked for).
inline bool Wanted(Mode m, const char* path, bool mapped) {
    if (mapped || m == Mode::off || !StartsWith(path, "ds/entities/weapons/") || std::strlen(path) <= 20) return false;
    return m == Mode::all || IsToolPath(path);
}

// A component resource of the weapon resource (its Decima type name): kept on the clone (1), dropped (0). Kept: the model,
// its parts and colours, the tool's own animation and the mover (all proven on mirrored gear entities at mirror level 1).
// Dropped: sound (no sounds from a copy), the anchor's build-check effect, and anything not known.
inline int KeepComponent(const char* type) {
    if (!type) return 0;
    static const char* const kKeep[] = {"SkinnedModelResource",
                                        "StaticModelResource",
                                        "ArtPartsDataResource",
                                        "ArtPartsVariationResource",
                                        "ArtPartsShaderVariableComponentResource",
                                        "ShaderVariableComponentResource",
                                        "StencilBufferComponentResource",
                                        "ExternalAnimationComponentResource",
                                        "DSSimpleAnimationComponentResource",
                                        "AttachToHelperMoverResource",
                                        "LodDistanceOverrideComponentResource",
                                        "BoostMeshStreamingForArtPartsDataResource",
                                        "MeshPaintTextureComponentResource"};
    for (const char* k : kKeep)
        if (std::strcmp(type, k) == 0) return 1;
    return 0;
}

// The prop key a weapon item gets on the receiver (partner_gear.cpp): its cutscene prop when one is mapped, else the held
// clone of its resource id when the mode wants it, else none (-1). `held_base` is the first held key.
inline int WeaponKey(Mode m, const char* path, int mapped_prop, unsigned res_id, unsigned max_res, int held_base) {
    if (mapped_prop >= 0) return mapped_prop;
    if (res_id >= max_res || !Wanted(m, path, false)) return -1;
    return held_base + static_cast<int>(res_id);
}

// A short name for the log and the dashboard ("ladder", "builder", "pile_lv2" ...).
inline const char* ShortName(const char* path) {
    if (!path) return "?";
    const char* s = std::strrchr(path, '/');
    return s ? s + 1 : path;
}

}  // namespace st::heldrules
