// Stranded Together textures: the slot table, the only game textures the mod may replace (with the user's edited
// pictures, docs/textures.md). Compiled into the extractor, the mod and the link check: kTableVersion (a CRC-32 of
// the rows) must match between them and the extracted folder's .manifest.txt.
// GUIDs, sizes and formats are identifiers read from the game data (Death Stranding Director's Cut) by the extractor's
// own parser (2026-11-22); no pixels are in the repo. A game update that changes any of them makes the extractor
// and the mod's live template check refuse, and the launcher asks for a new extraction.
#pragma once
#include <cstddef>
#include <cstdint>

#include "tex_codec.hpp"
#include "tex_hash.hpp"

namespace st::tex {

// logo: the Fragile logo rows (logo_slots.hpp, round 20), never in kSlots (their own table, outside kTableVersion)
enum class SlotGroup : std::uint8_t { suit, boots, logo };

struct Slot {
    const char* id;              // "S0".."S3", "B0".."B2"
    SlotGroup group;
    const char* set_core_path;   // the TextureSet's core path
    const char* set_guid;        // the TextureSet's ObjectUUID
    const char* object_guid;     // the Texture's ObjectUUID
    const char* content_guid;    // the Texture header's +0x10 GUID (the game's texture cache key)
    std::uint16_t w, h;
    std::uint8_t mips;           // the full chain
    std::uint8_t orig_fmt;       // EPixelFormat
    std::uint8_t srgb;           // header +0x0B
    std::uint32_t packing;       // the set entry's PackingInfo (R in the low byte)
    std::uint64_t views;         // the u64 after the streamed-mip count (BC7 / packed BC1 maps carry one)
    std::uint8_t clone_fmt;      // what the mod encodes the user's edit to (2.2's rule)
};

inline constexpr char kSuitSet[] = "ds/models/characters/fgl_fragile/core/fgl_textures/textures/fgl_body_suiti_v00_set";
// Her boots primitive draws Sam's porter-boot set (research-archive.md section 1).
inline constexpr char kBootSet[] = "ds/models/characters/sam_sam/core/sam_textures/textures/sam_body_boot_v00_set";

inline constexpr Slot kSlots[] = {
    // Fragile's porter suit (fgl_body_suiti_v00_set).
    {"S0", SlotGroup::suit, kSuitSet, "3acd953f-7cac-9547-b02b-af1af353d384", "2d99cdfc-7b94-c338-90e2-9d1c9ba236b6",
     "be2ca854-08f1-310d-9a95-0ecfb4a9f1f5", 2048, 2048, 12, kBC7, 1, 0x86211101u, 0x6DBull, kBC3},     // Colour RGB + Roughness A
    {"S1", SlotGroup::suit, kSuitSet, "3acd953f-7cac-9547-b02b-af1af353d384", "b450cb5a-892d-8332-b9fd-02f5499d6610",
     "2afa0960-b4e9-fa78-ed41-487690013387", 1024, 1024, 11, kBC7, 0, 0x85241404u, 0x6DBull, kBC3},     // Reflectance RGB + AO A
    {"S2", SlotGroup::suit, kSuitSet, "3acd953f-7cac-9547-b02b-af1af353d384", "15ec502c-6288-1730-a149-eca65466733c",
     "3510bf4d-4aa8-e589-eff8-ff481c38be45", 2048, 2048, 12, kBC5U, 0, 0x33231303u, 0x0ull, kBC5U},     // Normal RG
    {"S3", SlotGroup::suit, kSuitSet, "3acd953f-7cac-9547-b02b-af1af353d384", "569af893-c70a-3831-8495-48ae27d3c665",
     "380b1c86-86fd-afef-730b-1cc02220b6dc", 1024, 1024, 11, kBC1, 0, 0x80281808u, 0x0ull, kBC1},       // Mask RGB
    // Her boots (sam_body_boot_v00_set: Sam's porter boots).
    {"B0", SlotGroup::boots, kBootSet, "b9dfcdf0-e58f-844d-8ae7-c0ebeccd7d51", "94e9b870-bc60-ed3e-88cf-83a9013630c6",
     "7ba56a98-cb40-73fd-94a3-df53495136e1", 1024, 1024, 11, kBC5U, 0, 0x33231303u, 0x0ull, kBC5U},     // Normal RG
    {"B1", SlotGroup::boots, kBootSet, "b9dfcdf0-e58f-844d-8ae7-c0ebeccd7d51", "79e63885-d437-1633-840b-d6935a663118",
     "4cd346bb-d9fb-9bf6-b7c2-90a4e382f5e6", 1024, 1024, 11, kBC1, 1, 0x80211101u, 0x0ull, kBC1},       // Colour RGB
    {"B2", SlotGroup::boots, kBootSet, "b9dfcdf0-e58f-844d-8ae7-c0ebeccd7d51", "3a515eaf-1a73-e934-bbcc-e82f614f2e04",
     "a2fd50cd-80e2-4897-7f74-aa57c727a02d", 1024, 1024, 11, kBC1, 0, 0x80848685u, 0x0000064906000692ull, kBC1},   // AO R, Roughness G, Reflectance B
};
inline constexpr std::size_t kSlotCount = sizeof(kSlots) / sizeof(kSlots[0]);

// The table's identity: CRC-32 over every row's fields as text. Any change of a row changes it.
namespace detail {
inline constexpr char kSlotRowsText[] =
    "S0|suit|3acd953f-7cac-9547-b02b-af1af353d384|2d99cdfc-7b94-c338-90e2-9d1c9ba236b6|be2ca854-08f1-310d-9a95-0ecfb4a9f1f5|2048|2048|12|75|1|86211101|6db|68\n"
    "S1|suit|3acd953f-7cac-9547-b02b-af1af353d384|b450cb5a-892d-8332-b9fd-02f5499d6610|2afa0960-b4e9-fa78-ed41-487690013387|1024|1024|11|75|0|85241404|6db|68\n"
    "S2|suit|3acd953f-7cac-9547-b02b-af1af353d384|15ec502c-6288-1730-a149-eca65466733c|3510bf4d-4aa8-e589-eff8-ff481c38be45|2048|2048|12|71|0|33231303|0|71\n"
    "S3|suit|3acd953f-7cac-9547-b02b-af1af353d384|569af893-c70a-3831-8495-48ae27d3c665|380b1c86-86fd-afef-730b-1cc02220b6dc|1024|1024|11|66|0|80281808|0|66\n"
    "B0|boots|b9dfcdf0-e58f-844d-8ae7-c0ebeccd7d51|94e9b870-bc60-ed3e-88cf-83a9013630c6|7ba56a98-cb40-73fd-94a3-df53495136e1|1024|1024|11|71|0|33231303|0|71\n"
    "B1|boots|b9dfcdf0-e58f-844d-8ae7-c0ebeccd7d51|79e63885-d437-1633-840b-d6935a663118|4cd346bb-d9fb-9bf6-b7c2-90a4e382f5e6|1024|1024|11|66|1|80211101|0|66\n"
    "B2|boots|b9dfcdf0-e58f-844d-8ae7-c0ebeccd7d51|3a515eaf-1a73-e934-bbcc-e82f614f2e04|a2fd50cd-80e2-4897-7f74-aa57c727a02d|1024|1024|11|66|0|80848685|64906000692|66\n";
}  // namespace detail
inline constexpr std::uint32_t kTableVersion = Crc32(detail::kSlotRowsText, sizeof(detail::kSlotRowsText) - 1);

// The largest edited picture side the mod and Check my edits accept for a map of side pixels (twice the map, at most 8192):
// anything bigger is refused before it is decoded (a 16384 x 16384 PNG would need 1 GiB in the game's process).
inline constexpr std::uint32_t MaxEditSide(std::uint32_t side) { return side * 2 < 8192 ? side * 2 : 8192; }

// The slot whose original Texture has this ObjectUUID, or nullptr.
inline const Slot* SlotByObjectGuid(const char* guid) {
    for (const Slot& s : kSlots) {
        const char* a = s.object_guid;
        const char* b = guid;
        while (*a && *a == *b) ++a, ++b;
        if (*a == 0 && *b == 0) return &s;
    }
    return nullptr;
}

}  // namespace st::tex
