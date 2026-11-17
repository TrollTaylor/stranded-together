// The partner's real (solid, non-hologram) character bodies: the game's own
// cutscene-actor entities (ds/entities/cinematics/demo_*), each a small
// DSControlledEntityResource with SkinnedModel + ExternalAnimation and no AI.
// Paths and GUIDs come from the game data (tools/re/ds_archive.py,
// docs/game-data-index.md "Cutscene actor bodies"). Pure data and code: no
// Windows or game dependencies, because the launcher compiles this file too (its
// character list is the bodies with `offered`, so there is one source of truth).
// Unit-tested. See docs/real-partner-body.md.
#pragma once
#include <cstddef>
#include <cstdint>

namespace st::bodies {

struct Body {
    std::uint8_t id;         // wire body_id (1..), 0 = none
    const char* key;         // session.ini partner_character / my_character
    const char* label;       // shown in logs and the launcher
    const char* path;        // core path, without ".core"
    const char* guid;        // ObjectUUID as text (8-4-4-4-12)
    bool offered;            // proven in game (loaded, validated, posed): players may pick it
};

std::size_t Count();
const Body& At(std::size_t i);
const Body* FindByKey(const char* key);  // case-insensitive; nullptr when unknown
const Body* FindById(std::uint8_t id);
const Body& Default();                   // Sam in the porter suit

// The bodies a player may choose (launcher) and a receiver shows for a peer's body_id.
// The others stay in the table for debug spawns and tours until each is proven in game.
bool IsOffered(std::uint8_t id);         // false for 0 and unknown ids
std::size_t OfferedCount();
const Body& OfferedAt(std::size_t i);    // i < OfferedCount(), in table (id) order; else Default()

// "336f4328-1381-4947-8e6d-7def29e654bd" -> the 16 bytes Decima keeps in memory
// (GGUUID: the first three groups little-endian, the rest as written; Python
// uuid.UUID(text).bytes_le). False on a malformed string.
bool ParseGuid(const char* text, std::uint8_t (&out)[16]);

}  // namespace st::bodies
