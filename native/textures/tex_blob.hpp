// Stranded Together textures: the blob, one texture in the game's own serialized form (the bytes the game's texture
// loader 0x141D24460 reads), fully embedded: every mip in the blob, nothing streamed. Built by the guest's mod from the
// user's edited pictures, sent over the link as is, validated strictly by the receiver before the game sees a byte.
//
//   +0x00 u8 0 (2D)  +0x02 u16 w  +0x04 u16 h  +0x06 u16 0  +0x08 u8 mips (the full chain)  +0x09 u8 format
//   +0x0B u8 sRGB  +0x0D u8 0xA9 (as every game texture)  +0x0E u8 0 (no streaming mips)  other header bytes 0
//   +0x10 GUID = "STTX" + SHA-256(the blob with the GUID zeroed)[0..11]
//   +0x20 u32 20 + embedded  +0x24 u32 embedded  +0x28 u32 0 (streamed)  +0x2C u32 0 (first embedded mip)
//   +0x30 u64 the slot's extra views  +0x38 the mips, largest first, tight (at least one block each)
// The blob id is the SHA-256 of the whole blob. Pure code (BCrypt for the hash), no game, no files.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "tex_codec.hpp"
#include "tex_hash.hpp"
#include "tex_slots.hpp"

namespace st::tex {

inline constexpr std::size_t kBlobHeader = 0x38;
inline constexpr std::uint32_t kBlobEncoderVersion = 1;   // bump when the encoder's output changes (cache keys)
inline constexpr std::size_t kBlobMax = 6u * 1024u * 1024u;

// The slot's index in kSlots, or -1.
int SlotIndex(const char* id);
// How the slot's mips are averaged: sRGB colour, a normal, or linear.
MipKind SlotMipKind(const Slot& s);
// Bytes of a blob for the slot (header + the full chain in clone_fmt).
std::size_t BlobSize(const Slot& s);

// `packed` is the slot's texture as RGBA8 in the slot's own channel layout (tex_codec MapsOf / InsertChannels), any size
// (resized to the slot's size). Builds the full mip chain, encodes it in the slot's clone format and writes the blob.
bool BuildBlob(const Slot& s, const Image& packed, std::vector<std::uint8_t>& blob, std::string* error = nullptr);
// Strict check of a blob for the slot: every header byte, the sizes, the views, the GUID (recomputed) and, when `id` is
// given, the SHA-256 of the whole blob. False + the first reason.
bool ValidateBlob(const Slot& s, const std::uint8_t* p, std::size_t n, const Sha256* id, std::string* why = nullptr);
// The GUID bytes the blob must carry at +0x10 (computed with that field zeroed).
bool BlobGuid(const std::uint8_t* p, std::size_t n, std::uint8_t out[16]);

// The 16 bytes of a GUID as the game stores it, from the text form ("2d99cdfc-7b94-c338-90e2-9d1c9ba236b6", the
// extractor's GuidText: Windows byte order). False for malformed text.
bool GuidBytes(const char* text, std::uint8_t out[16]);

}  // namespace st::tex
