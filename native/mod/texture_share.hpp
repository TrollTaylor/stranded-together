// Fragile's edited textures over the link (STT1 on channel 5, texture_wire.hpp; docs/textures.md, docs/real-partner-body.md
// section 29). The guest sends its look (which blobs her suit wears); the host answers which blobs it already has (its
// disk cache) and which it wants; the guest sends the wanted ones in 980-byte pieces, paced (about 128 KB/s in the world,
// 400 KB/s at the title or on a loading screen, nothing while the save channel is busy). The host validates every blob
// strictly against its own slot table (tex_blob ValidateBlob) before anything else sees it, caches it (LRU 256 MB) and
// shows the look once all its blobs are there; until then the previous look (or her own). Game thread (player_sync's tick).
#pragma once
#include <string>

#include "fragile_textures.hpp"

namespace st::texshare {

struct Settings {
    bool on = false;          // fragile_textures=1 and a host or a guest
    bool guest = false;
    bool send = true, receive = true;
    bool tint = false;        // guest: this game's fragile_suit_colour (LOOK flags bit 1, so the host draws the same)
    std::wstring cache;       // the host's partner cache folder (<mod dir>\cache\partner-textures, a hidden run's OutDir)
};
void Configure(const Settings& s) noexcept;
void Tick(double now_ms, bool in_world) noexcept;
// A new link session (player_sync): the host forgets the previous guest's look, boots style, tint and half-received
// blobs (the new guest sends its own LOOK; one that sends none shows her own look).
void NewSession(std::uint32_t generation) noexcept;
// Guest: the look her own Fragile wears now (have = settled), sent to the host on every change.
void SetOwnLook(bool have, const ftex::Look& look, int colour) noexcept;
// Guest: her boots (docs/real-partner-body.md section 30): the boot type (-1 unknown), worn, and the boots style (own:
// her own boots, no prop on the host's screen). Sent in the LOOK (flags bit 0, boots, worn); a change sends it again.
void SetOwnBoots(int type, bool worn, bool own_style) noexcept;
// Host: the guest's boots style from its newest LOOK (flags bit 0): -1 none received yet, 0 prop, 1 own.
int PartnerBootsStyle() noexcept;
// Host: the guest's v0.23 tint choice from its newest LOOK (flags bit 1): -1 none received yet, 0 off, 1 on.
int PartnerTint() noexcept;
// Host: the guest's newest complete look (always true; empty = her own look or nothing received).
bool PartnerLook(ftex::Look* out) noexcept;
void LogStatus() noexcept;

}  // namespace st::texshare
