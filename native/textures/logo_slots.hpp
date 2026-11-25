// Stranded Together textures: the Fragile logo's slot rows (round 20, docs/textures.md "The Fragile Express logo").
// The game textures the logo copies replace: Sam's porter-suit hood maps (the hood drawn on her exo carrier) and the
// Bridges cap's colour and normal maps. Their own table, apart from kSlots: they never go over the link (each screen makes
// them from its own extracted pictures) and are not part of kTableVersion (an extracted folder of an older mod stays good).
// Identifiers only (GUIDs, sizes, formats, read from the game data by the extractor's parser and tools/re); no pixels.
// The per-drawable override keeps a logo texture only in slots whose original is exactly that row's texture (the rule
// "exact", fragile_textures.cpp), so a palette hood, a metal hood and each cap variant each take only their own copy.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "tex_slots.hpp"

namespace st::tex {

inline constexpr char kSamSuitV00[] = "ds/models/characters/sam_sam/core/sam_textures/textures/sam_body_suiti_v00_set";
inline constexpr char kSamSuitV02[] = "ds/models/characters/sam_sam/core/sam_textures/textures/sam_body_suiti_v02_set";
inline constexpr char kSamSuitV03[] = "ds/models/characters/sam_sam/core/sam_textures/textures/sam_body_suiti_v03_set";
inline constexpr char kCap00[] = "ds/models/characters/sam_sam/core/sam_textures/textures/sam_part_cap00_set";
inline constexpr char kCap00V01[] = "ds/models/characters/sam_sam/core/sam_textures/textures/sam_part_cap00_v01_set";
inline constexpr char kCap00V02[] = "ds/models/characters/sam_sam/core/sam_textures/textures/sam_part_cap00_v02_set";

// Row order is the logo slot number (kLogoBase + i in the mod's slot numbering).
enum LogoSlot : int {
    kHoodColour, kHoodMask, kMetalColour, kMetalMask, kHoodNormal, kCapColour, kCapV01Colour, kCapV02Colour, kCapNormal, kSilverRefl, kGoldRefl,
    kLogoSlotCount
};

inline constexpr Slot kLogoSlots[] = {
    // Sam's porter suit, the palette set (v00): its hood band carries BRIDGES (rotated 180 degrees in the map).
    {"L0", SlotGroup::logo, kSamSuitV00, "237ca9d6-68a4-424f-b79e-d3e8c85ecc2b", "561f472a-581c-cf3f-a502-f432ed7d986d",
     "32dc6fcd-3e5e-85da-4d2e-6ff8e0aabc04", 2048, 2048, 12, kBC1, 1, 0x80211101u, 0x0ull, kBC1},              // Colour RGB
    {"L1", SlotGroup::logo, kSamSuitV00, "237ca9d6-68a4-424f-b79e-d3e8c85ecc2b", "241c9660-caaf-5534-86f4-41bd8f66c5ec",
     "1dcfc9fc-4f97-eca7-4b3e-f26dbd52f28c", 2048, 2048, 12, kBC1, 0, 0x80808689u, 0x6000649ull, kBC1},       // Mask alpha R, Roughness G
    // The silver (v02) and gold (v03) sets share these two textures' content (one copy serves both).
    {"L2", SlotGroup::logo, kSamSuitV02, "635ddaba-b020-1849-935a-63dfd2dce1e7", "567388d9-0306-d937-a768-3702e7e6b0a1",
     "be8f91de-3e09-fac6-d55b-c76a9bd91a95", 2048, 2048, 12, kBC1, 1, 0x80211101u, 0x0ull, kBC1},              // Colour RGB
    {"L3", SlotGroup::logo, kSamSuitV02, "635ddaba-b020-1849-935a-63dfd2dce1e7", "f3a4a263-cf0f-0438-a9dd-df97417879fe",
     "b7851a64-8889-1828-c638-1904dd2b6e13", 2048, 2048, 12, kBC1, 0, 0x80808689u, 0x6000649ull, kBC1},       // Mask alpha R, Roughness G
    // The normal map: one content for v00 / v02 / v03.
    {"L4", SlotGroup::logo, kSamSuitV00, "237ca9d6-68a4-424f-b79e-d3e8c85ecc2b", "9c357b3b-55b5-033e-a519-c34f7e74e03e",
     "5f147acf-8688-8f5d-76ec-7a26cb9093ef", 2048, 2048, 12, kBC5U, 0, 0x33231303u, 0x0ull, kBC5U},           // Normal RG
    // The Bridges cap (cap00): the palette set, the two metal variations, and the palette set's normal map. Only L5 and L8
    // are made and bound (round 20): the metal sets' own normal and reflectance / roughness maps carry BRIDGES as well, so
    // L6 / L7 stay rows only (kept for a later round; silver / gold caps keep BRIDGES).
    {"L5", SlotGroup::logo, kCap00, "c322fa4a-56e0-ea49-b090-4a790c710bbf", "ce15d3b0-762c-e039-a95c-f0279f7a4fb1",
     "52fdf18c-a0f4-d012-bc5f-766bb880b403", 1024, 1024, 11, kBC7, 1, 0x89211101u, 0x6DBull, kBC3},           // Colour RGB + Mask alpha A
    {"L6", SlotGroup::logo, kCap00V01, "d0facd2b-862f-f241-9ba7-8390789c1866", "c4ded47f-6cfb-e634-b225-dbd7883a8fdc",
     "87a9dbbc-e86f-f735-20b9-b076e1a22c51", 1024, 1024, 11, kBC7, 1, 0x84211101u, 0x6DBull, kBC3},           // Colour RGB + Reflectance A
    {"L7", SlotGroup::logo, kCap00V02, "eccf558d-a5ff-924a-8aa3-be0fa642d852", "533fac9f-bb12-c638-8c36-44520a074d9d",
     "25819621-5a36-fd59-965a-bbca92fca632", 1024, 1024, 11, kBC1, 1, 0x80211101u, 0x0ull, kBC1},             // Colour RGB
    {"L8", SlotGroup::logo, kCap00, "c322fa4a-56e0-ea49-b090-4a790c710bbf", "02f576b5-fd13-b430-8931-11eb82b0a82c",
     "98fa95cb-384b-2c7c-2c19-4f3cfc1cef07", 1024, 1024, 11, kBC5U, 0, 0x33231303u, 0x0ull, kBC5U},           // Normal RG
    // The metal hoods' reflectance (silver v02, gold v03: their own contents): BRIDGES is a print that does not shine.
    {"L9", SlotGroup::logo, kSamSuitV02, "635ddaba-b020-1849-935a-63dfd2dce1e7", "cf5ef8b8-d27d-5f3b-ab4a-7f17749d4d1e",
     "bc6b9fc8-82d0-9c5f-41d4-49cb4beedf7a", 1024, 1024, 11, kBC7, 0, 0x85241404u, 0x6DBull, kBC3},           // Reflectance RGB + AO A
    {"L10", SlotGroup::logo, kSamSuitV03, "e8fa5c47-d13c-d443-8386-aaf16767fc94", "5ee41bfa-fef1-2d36-ae68-1e362acb027c",
     "a121e102-0c68-a07d-ae61-83b59b7b0689", 1024, 1024, 11, kBC7, 0, 0x85241404u, 0x6DBull, kBC3},          // Reflectance RGB + AO A
};
static_assert(sizeof(kLogoSlots) / sizeof(kLogoSlots[0]) == kLogoSlotCount, "one row per LogoSlot");

// The mod's slot numbering: 0..kSlotCount-1 the suit and boots rows (kSlots, the link's numbers), then the logo rows.
inline constexpr int kLogoBase = static_cast<int>(kSlotCount);
inline constexpr int kAnySlotCount = kLogoBase + kLogoSlotCount;
inline const Slot* AnySlot(int i) {
    if (i >= 0 && i < kLogoBase) return &kSlots[i];
    if (i >= kLogoBase && i < kAnySlotCount) return &kLogoSlots[i - kLogoBase];
    return nullptr;
}

// The cap's colours 0..22 are its palette set (v00), the one the logo is made for. 23 (silver) and 24 (gold) are the v01 /
// v02 sets: they keep v00's colour + mask alpha texture for the mask alpha and its normal map but have their own colour
// and reflectance with BRIDGES, so the logo there mixed with BRIDGES (round 20's two-game stills): they keep BRIDGES.
// 0xFF (not known) too.
inline constexpr std::uint8_t kCapPaletteColours = 23;
inline constexpr bool CapLogoFits(std::uint8_t cap_colour) { return cap_colour < kCapPaletteColours; }

// ---- the binding's pure parts (fragile_textures.cpp, texture_looks.cpp; unit-tested in st_logo_tests; review r20 visual 13)

// The hood_any look: the palette look's rows, then the metal look's rows that are not in it already (L4, the normal map both
// use, once), sorted by slot: the look id's order, the same whichever order the two looks were made in (cached or built).
// Each result: {slot, the row's index in a (0..a.size()-1) or a.size() + its index in b}.
inline std::vector<std::pair<int, std::size_t>> MergeLookRows(const std::vector<int>& a, const std::vector<int>& b) {
    std::vector<std::pair<int, std::size_t>> rows;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::none_of(rows.begin(), rows.end(), [&](const auto& r) { return r.first == a[i]; })) rows.push_back({a[i], i});
    for (std::size_t i = 0; i < b.size(); ++i)
        if (std::none_of(rows.begin(), rows.end(), [&](const auto& r) { return r.first == b[i]; })) rows.push_back({b[i], a.size() + i});
    std::stable_sort(rows.begin(), rows.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    return rows;
}

// The exo carrier's mesh mask (meshes 0..255): the meshes whose probe entry met a logo texture; the look is bound on those only.
using MeshMask = std::uint64_t[4];
inline bool MaskHas(const MeshMask& m, int mesh) { return mesh >= 0 && mesh < 256 && ((m[mesh >> 6] >> (mesh & 63)) & 1u) != 0; }
// True when it was added now (a mesh already in, or out of 0..255: false).
inline bool MaskAdd(MeshMask& m, int mesh) {
    if (mesh < 0 || mesh >= 256 || MaskHas(m, mesh)) return false;
    m[mesh >> 6] |= 1ull << (mesh & 63);
    return true;
}
inline bool MaskEmpty(const MeshMask& m) { return !(m[0] | m[1] | m[2] | m[3]); }
inline unsigned MaskCount(const MeshMask& m) {
    unsigned n = 0;
    for (int i = 0; i < 256; ++i) n += MaskHas(m, i) ? 1u : 0u;
    return n;
}

// The overrides a look should need before the core is asked: one entry per map of each slot per bound mesh, at most `cap` (the
// registry's room is checked against it). The core's own count can differ (a mesh holding a map twice, or none): see
// SlotFits.
inline unsigned OverrideEstimate(const unsigned* maps_per_slot, std::size_t slots, unsigned bound_meshes, unsigned cap) {
    unsigned long long need = 0;
    for (std::size_t i = 0; i < slots; ++i) need += static_cast<unsigned long long>(maps_per_slot[i]) * bound_meshes;
    return need > cap ? cap : static_cast<unsigned>(need);
}
// After the core made `made` entries for one more slot on top of `kept` already kept: the slot stays bound only when all of
// them fit `cap` (else the slot is undone whole and the look keeps its leading slots).
inline constexpr bool SlotFits(unsigned kept, unsigned made, unsigned cap) { return kept <= cap && made <= cap - kept; }

}  // namespace st::tex
