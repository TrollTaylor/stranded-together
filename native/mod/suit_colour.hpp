// Fragile's porter suit in Sam's suit colours (docs/real-partner-body.md section 27).
//
// Her suit (demo_fgl_body_suiti, body key fragile_suit) has no colour variants in the game and her shader ignores the
// suit-colour variable, so the colour is put on at runtime the way the game puts Sam's silver / gold suit on: the game's
// own per-channel texture-set override of a model's drawable (0x14237CA20, the core of ApplyArtPartsVariation's
// ReplaceTextureSet), here with a small TextureSet of the mod's own whose channels (Color, Reflectance, Roughness) are
// 1x1 constant textures built by the game's own texture loader (0x141D24460) from 4 colour bytes (suit_colour_math.hpp).
// Nothing is read from or written to files, the save or the game state; the overrides live on the mod's own spawned
// bodies only and are taken off again with the game's undo (0x14237A960).
//
// Game thread only (real_body's tick); every game call SEH-wrapped; every address byte-checked once (Available), a
// mismatch or a fault turns the feature off for the session with one log line.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace st::suitcol {

bool Available() noexcept;       // byte checks (lazy, once, logged)
const char* Why() noexcept;      // the reason when not available
void Configure(bool on) noexcept;  // session.ini fragile_suit_colour
bool Enabled() noexcept;         // configured on and available
// Round 5: the guest's own pick of Sam's suit colour, remembered across the host's loads (suit_colour_math GuestPick) in
// fragile_suit_colour.txt in `dir` (her own mod folder; a hidden run's own output folder). `on`: a guest in a host-held save
// session with fragile_suit_colour on. GuestColour(her Sam's colour) -> the colour Fragile wears (game thread).
void ConfigureGuestPick(bool on, const std::wstring& dir) noexcept;
std::uint8_t GuestColour(std::uint8_t cur) noexcept;
// Round 15 (docs/real-partner-body.md section 40), game thread, every tick with the local player (`player` 0 when none):
// her Sam's suit colour byte (inventory +0x4BE1) at `byte_addr`, read now as `byte_now` (-1 unreadable); `kind6` the
// subspace kind this tick (1 kind 6, 0 other, -1 unreadable). The wardrobe's close is her pick (the colour shown counts
// too); with `real_outfit 9` bit 1 her Sam's byte is set to her pick 1.5 s into a ready world while the wardrobe is
// closed (at most 5 writes in 10 s, logged), on a sandboxed (redirected) save only.
void GuestTick(std::uintptr_t player, std::uintptr_t byte_addr, int byte_now, int kind6, double now_ms) noexcept;
void NotePick(std::uint8_t c) noexcept;     // the test action `real_outfit 4 c`: a pick
// The test action `real_outfit 12` (hidden runs): the open wardrobe counts as if a suit colour had been previewed (a save
// without suit colours unlocked shows none). False when the wardrobe is not open.
bool MarkPreview() noexcept;
void SetAlign(bool on) noexcept;            // off: the world's own byte put back when it still holds ours
// The world's own byte put back when it still holds what the alignment wrote (the emergency stop; dllmain). Only while the
// same world is ready and the byte was seen in the last 2 s (the address belongs to that world's Sam). Game thread or the
// fault path; never throws.
void PutBack() noexcept;
bool AlignOn() noexcept;
const char* DescribeGuest(char* buf, std::size_t cap) noexcept;

// What has been done to one body. Reset (Forget) when the body goes.
struct Dress {
    std::uintptr_t e = 0;          // the body these fields describe
    std::uintptr_t drawable = 0;   // its model drawable when the overrides were bound
    std::uint8_t applied = 0;      // the colour bound now (0 = her own textures, nothing bound)
    std::uint8_t pending = 0xFE;   // the colour wanted since pending_ms (a change waits until it holds)
    double pending_ms = 0;
    double next_ms = 0;            // rate limit between steps
    std::uint64_t handles[96] = {};  // the game's override handles (slot, mesh) to undo
    unsigned nh = 0;
    unsigned applies = 0, undos = 0, binds = 0;
    std::uint8_t refused_logged = 0xFE;
};

// Brings `e` (a live fragile_suit body of ours) towards `colour` (0..16; 0 = her own look), one step per call and at most
// one step per 250 ms; a change must hold 0.5 s first. Logged.
// `tint`: -1 this game's fragile_suit_colour, 0 / 1 the other player's choice (the host drawing the guest's Fragile).
void Apply(Dress& d, std::uintptr_t e, std::uint8_t colour, double now_ms, int tint = -1) noexcept;
// The body is still alive: take our overrides off now (game thread). Logged.
void Undo(Dress& d, const char* why) noexcept;
void Forget(Dress& d) noexcept;  // the body is gone: its drawable took the overrides with it, nothing to call

// Debug / hidden tests (real_outfit 6 / 7): the body's drawable and meshes to the log; which meshes and channels the next
// binds use (mesh_mask -1: the product's choice; bit i: mesh i; -2: the whole model at once, mesh index -1; channels 0:
// the product's choice, else Channel bits).
void LogBody(std::uintptr_t e, const char* what) noexcept;
void SetTest(long long mesh_mask, unsigned channels) noexcept;
void SetTestMetal(int variant, int roughness) noexcept;  // real_outfit 8: the metals' look (suit_colour_math kMetals)
const char* Describe(const Dress& d, char* buf, std::size_t cap) noexcept;

}  // namespace st::suitcol
