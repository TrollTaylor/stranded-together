// Fragile's textures, the file side (docs/textures.md): a worker thread ("tex-build", no game calls, below-normal
// priority) that reads the user's textures folder (its .manifest.txt from the launcher's extraction), finds the pictures
// the user edited in the folder of the guest's Sam suit colour, packs them into her suit's textures (tex_slots S0..S3:
// the edited picture plus her originals for the other channels), builds the full mip chain, BC-encodes it and writes the
// blob (tex_blob) - cached in the mod's own cache\built-textures (never the textures folder) so the next start is instant. Only files that differ from what the
// extractor wrote count as edited; an untouched folder is her own look. A saved PNG is picked up again after it settles
// (1 s, it must decode). Also the in-memory test patterns (tex 9: no files, no game data).
// Round 8 (ready_made_rules.hpp, docs/textures.md "Ready-made colours"): a colour folder 01..16 with nothing painted
// wears Stranded Together's ready-made colour, made here from her Original pictures (suit_defaults.hpp, cached like any
// built slot); a painted folder (any of the six pictures the mod reads) wears exactly its pictures. The ready-made
// blobs are announced to the host as a RECIPE (texture_share): a host with the same Original pictures makes them itself
// on this worker (MakeForPartner).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "fragile_textures.hpp"

namespace st::ftex::looks {

void Configure(const Settings& s) noexcept;
void SetWantedColour(int colour) noexcept;     // game thread: the colour the guest's Fragile wears (0..16)
// Game thread: the boots look she wears (bootslook::Key: the boot type's folder, worn, metal; -1 none).
void SetWantedBoots(int key) noexcept;
// The guest's own look for `colour` and the boots look `boots_key` (-1: no boots slots), once both are built (an empty
// look = her own textures): her suit's edited slots, then her boots'. False while not settled.
bool OwnLook(int colour, int boots_key, Look* out) noexcept;
void SetPattern(int pattern) noexcept;         // tex 9: 0 off, 1 suit checker, 2 suit checker + boots stripes, 3 boots stripes
bool PatternLook(Look* out) noexcept;          // the pattern's look once built
void Rescan() noexcept;
void Resend() noexcept;                         // tex 7: the guest resends its look; the host lets go of the RAM copies (texture_share)
bool TakeResend() noexcept;
unsigned BuiltCount() noexcept;
void Describe(char* buf, std::size_t cap) noexcept;
// The look id of a list of {slot, blob id} (sorted by slot): SHA-256 of the bytes slot, id, slot, id...
void LookId(Look& l) noexcept;

// ---- Round 8: the ready-made colours
struct Recipe {
    int slot = -1, colour = 0, variant = 0;
    std::uint8_t originals[32] = {};   // suit_defaults.hpp OriginalsId of the pictures it was made from
    bool valid = false;
};
// Guest: the blob `id` of ours is a ready-made slot (what the RECIPE says about it).
bool RecipeFor(const std::uint8_t id[32], Recipe* out) noexcept;
// tex 11: the metal variant (suit_defaults.hpp kMetalVariants; 0 the product). Rebuilds the looks.
void SetVariant(int v) noexcept;
int Variant() noexcept;
// Host: whether this game can make a guest's ready-made blob (a textures folder, the worker, tex 12 not 1). The worker
// checks the rest (the manifest, her Original pictures, their identity) and reports a failure through TakeMade.
bool CanMake() noexcept;
// tex 12: 0 product, 1 never make here (always ask), 2 pretend the originals differ, 3 every make waits 2 s first, 4 the
// next make throws (out of memory)
int MakeMode() noexcept;
void SetMakeMode(int m) noexcept;
// Host: make the guest's ready-made blob `id` here (the worker, never the game thread). Once made and checked by its
// SHA-256 it is written to `partner_cache` (then pruned to `cache_max` bytes, oldest first) and stored (StoreReceived);
// the result comes back through TakeMade: exactly one per request the worker takes, also when making it throws. False
// when the request could not be queued (out of memory): ask the guest instead.
bool MakeForPartner(const Recipe& r, const std::uint8_t id[32], const std::wstring& partner_cache, std::uint64_t cache_max) noexcept;
// Host: queued requests for blobs that are none of `keep` (the newest LOOK's n ids) are dropped without a result (the
// one being made finishes and reports). Returns how many were dropped.
unsigned DropQueuedMakes(const std::uint8_t (*keep)[32], unsigned n) noexcept;
struct Made {
    std::uint8_t id[32] = {};
    bool ok = false;
    unsigned ms = 0;
    char why[96] = {};
};
bool TakeMade(Made* out) noexcept;     // game thread: one finished request (none is ever dropped)

// ---- Round 20: the Fragile Express logo (logo_recipe.hpp, docs/textures.md "The Fragile Express logo")
// Game thread: whether this screen shows the logo (a carrier or cap of hers wants it). The worker then makes the copies
// once, from this game's own extracted pictures (host and guest alike: no logo texture goes over the link).
void SetLogoWanted(bool on) noexcept;
enum class LogoPart : int { hood_palette, hood_metal, hood_any, cap };
// The logo look for `part` (hood_any: both hood sets, for a carrier whose colour row is not known) once made. False: not
// made yet, or it cannot be (no textures folder, an extraction without the logo's pictures: logged once).
bool LogoLook(LogoPart part, Look* out) noexcept;

}  // namespace st::ftex::looks
