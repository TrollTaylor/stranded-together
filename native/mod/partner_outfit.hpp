// The partner Sam's outfit: dress the partner's demo_sam_clean body like the other player's own
// Sam (docs/real-partner-body.md section 9).
//
// What makes Sam's look, and what is matched:
//   - the "exos" art-parts variation (0..43) of the porter suit: silver / gold suit textures, the
//     power-glove and exoskeleton tints. The game keeps the applied index at player +0x44B0
//     (mesh controller +0xA60, +0x3A50) and creates it from the player's current body variant
//     (streaming control +0x78 -> DSPlayerBodyVariant +0x48 count / +0x50 ArtPartsVariationResource*).
//     The receiver takes the same resource from ITS OWN player's porter-suit variant (the data are
//     identical in both games) and adds it to the body with the game's ApplyArtPartsVariation.
//   - the suit colour 0..14: a shader variable (hash 0x2967E719) on every model part, set with the
//     game's own "apply suit colour" function (the uniform menu's preview uses it).
//   - demo_sam_clean carries one variation of its own (apv_sam_body_suiti_v01, a darker suit
//     texture set) that the player's suit does not: it is removed first (the game's component
//     removal puts the original texture set back).
// Not matched: the body variant (MULE suit, private-room wear: the body stays the porter suit),
// the cap and glasses (separate entities), the exoskeleton / glove meshes, backpack and cargo.
//
// Fault-free reads (partner::mem) everywhere; every game function and offset is byte-checked once
// (Available). Game calls run on the game thread only (real_body Tick), SEH-wrapped; a fault turns
// the feature off for the session.
#pragma once
#include <cstddef>
#include <cstdint>

#include "pose_wire.hpp"

namespace st::outfit {

// Byte checks (lazy, once, logged). False: no outfit is read or applied this session.
bool Available() noexcept;

// The local player's outfit (fault-free reads; the player entity, its inventory and streaming
// components are checked). False when the player does not check out.
bool ReadLocal(std::uintptr_t player, wire::Outfit* out) noexcept;

// The local player wears the porter suit (its body variant's GUID when loaded, else its index): the
// suit whose ArtParts (part_sam_body_suiti) demo_sam_clean also has. False when unsure.
bool LocalWearsPorter(std::uintptr_t player) noexcept;

// What has been done to one body (game thread). Reset (Forget) when the body goes.
struct Dress {
    std::uintptr_t e = 0;             // the body these fields describe
    bool v01_done = false;            // the body's own suit variation removed (or there was none)
    std::uint8_t exos = 0xFF;         // the exos variation added (0xFF none)
    std::uintptr_t exos_res = 0;      // its ArtPartsVariationResource (owned by our player's variant)
    std::uint8_t colour = 0xFF;       // the suit colour set (0xFF never set, or reset by a variation change)
    std::uint8_t pending_exos = 0xFE; // the exos index wanted since pending_ms (a change waits until it holds)
    double pending_ms = 0;
    std::uint8_t want_logged_exos = 0xFE, want_logged_colour = 0xFE;
    unsigned applies = 0;             // game calls made on this body
    unsigned colour_sets = 0;         // suit colour calls on this body (logged: a test can tell a repeat)
    double next_ms = 0;               // rate limit between changes
};

// Brings the body `e` (a live demo_sam_clean) towards `want`, at most one change step per call
// and one call per 250 ms. `local` = this game's own player (the source of the variation
// resources). Logged.
void Apply(Dress& d, std::uintptr_t e, const wire::Outfit& want, std::uintptr_t local, double now_ms) noexcept;
void Forget(Dress& d) noexcept;  // the body is gone: nothing to call

// Hidden tests only (the caller checks automation + redirected scratch saves): our own Sam's suit
// colour byte (inventory +0x4BE1, 0..16) is overwritten, as the uniform menu would; the game's
// inventory update then re-dresses Sam itself. Logged.
bool TestSetOwnSuitColour(std::uintptr_t player, std::uint8_t colour) noexcept;

// ---- Sam's clothes change (docs/real-partner-body.md section 34; room_outfit_rules.hpp): his streaming control's switch
// state and variants, and his paint effect's init (the 60 s wait behind the private room's black screen). Byte-checked on
// their own (ClothesAvailable, lazy, once, logged): a mismatch turns only the clothes rule and the room outfit's variant
// rule off.
bool ClothesAvailable() noexcept;
const char* ClothesWhy() noexcept;
struct VariantState {
    std::int32_t state = -1;                      // sc +0x70: 0 idle, 1 streaming, 2 apply, 3 the paint wait
    std::uint8_t cur = 0xFF, wanted = 0xFF;       // sc +0x74 / +0x75
    std::uint8_t request = 0xFF;                  // sc +0x76 (8: the node-graph variable)
    float delay = 0, wait = 0;                    // sc +0x98 (state 3's 1 s), +0x9C (the 120 s give-up)
    bool paint_ok = false;                        // the paint effect (player +0x54F0) checked out and was read
    bool paint_ready = false;                     // as 0x142707090: +0xAA2C, or +0x38 and +0xAA21 == 3
    std::uint8_t paint_stage = 0xFF;              // +0xAA21 (0..3)
    std::uint8_t paint_released = 0xFF;           // +0xAA2C (the 60 s fallback sets it)
    std::uint8_t paint_flags = 0xFF;              // +0xAA2E (bit 0: the fallback fired)
    float paint_timer = -1;                       // +0xAA28 (s; > 60 releases)
    int loading_model = -1;                       // the named flag DSPlayerLoadingModel (-1: not read)
};
// Fault-free reads of the local player's streaming control (and paint effect when `paint`); false when unavailable or the
// player / component does not check out (type, owner).
bool ReadVariantState(std::uintptr_t player, VariantState* out, bool paint) noexcept;
// "variant 0 -> 1 (request 8, state 3, 1 s delay 0.0, wait 0.4 s); paint ready (stage 3, timer 0.4 s, released 0, flags 0x00); DSPlayerLoadingModel 1"
const char* DescribeVariantState(const VariantState& v, char* buf, std::size_t cap) noexcept;
// Hidden tests only (the caller checks automation + scratch saves): writes the request byte sc +0x76 (0..7 a variant, 8
// the node-graph variable), as the room's uniform menu does. Logged.
bool TestRequestVariant(std::uintptr_t player, std::uint8_t request, char* msg, std::size_t cap) noexcept;

// Debug: the local player's outfit and the body's art-parts variations, to the log.
void LogLocal(std::uintptr_t player) noexcept;
void LogBody(std::uintptr_t e, const char* what) noexcept;
const char* Describe(const wire::Outfit& o, char* buf, std::size_t cap) noexcept;

}  // namespace st::outfit
