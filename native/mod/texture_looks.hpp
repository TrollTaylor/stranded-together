// Fragile's textures, the file side (docs/textures.md): a worker thread ("tex-build", no game calls, below-normal
// priority) that reads the user's textures folder (its .manifest.txt from the launcher's extraction), finds the pictures
// the user edited in the folder of the guest's Sam suit colour, packs them into her suit's textures (tex_slots S0..S3:
// the edited picture plus her originals for the other channels), builds the full mip chain, BC-encodes it and writes the
// blob (tex_blob) - cached in the mod's own cache\built-textures (never the textures folder) so the next start is instant. Only files that differ from what the
// extractor wrote count as edited; an untouched folder is her own look. A saved PNG is picked up again after it settles
// (1 s, it must decode). Also the in-memory test patterns (tex 9: no files, no game data).
#pragma once
#include <cstddef>

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

}  // namespace st::ftex::looks
