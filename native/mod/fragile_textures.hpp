// Fragile's porter suit in the user's own edited textures (docs/textures.md, docs/real-partner-body.md section 29).
//
// The launcher extracts her suit's maps into the user's textures folder (native/textures); the user paints one set per
// Sam suit colour. This module turns the edited PNGs into game textures (a worker thread: decode, pack, mips, BC encode,
// the blob of tex_codec), makes them with the game's own texture loader (0x141D24460, as v0.23's 1x1 constants) and binds
// them on the mod's own fragile_suit bodies with the game's per-drawable texture-set override (0x14237CA20) - ONLY on her
// suit's slots: the override's rebuild (0x141DF4720) is detoured so our texture goes into the slots that hold one of her
// suit's own textures, never into her boots, neck, badge or the other parts of the same mesh.
//
// Game thread for every game call (real_body's tick), SEH-wrapped; addresses byte-checked once (Available), a mismatch or
// a fault turns the feature off with one line. The detour runs on whatever thread the game updates drawables on and only
// reruns, on the game's own table inside the game's own call, the two functions the original itself uses.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace st::ftex {

bool Available() noexcept;          // byte checks (lazy, once, logged)
const char* Why() noexcept;

// session.ini: fragile_textures, textures_dir (already checked against the automation rule by the caller), hot reload,
// the guest's role (only a guest builds a look from files; the host shows what the link brings).
struct Settings {
    bool on = true;
    bool hot_reload = true;
    bool free_unused = true;  // textures_free: a texture of ours unused for 5 s is released (the game's own Texture release)
    bool send = true;
    bool receive = true;
    bool guest = false;
    std::wstring dir;    // the textures folder ('' = none)
    // where the built textures are cached (<mod dir>\cache\built-textures; a hidden run: <run>\textures-cache). Never
    // inside the textures folder: that one may be synced to the cloud, and the folder watch would see our own writes.
    std::wstring cache;
    // Round 8: the ready-made colours (session.ini textures_defaults, default on): a colour folder 01..16 with nothing
    // painted wears Stranded Together's ready-made colour; off: her own suit there (the v0.24 rule)
    bool defaults = true;
    int defaults_variant = 0;   // the metal variant (tex 11; 0 the product)
};
void Configure(const Settings& s) noexcept;
bool Enabled() noexcept;            // configured on, available, a folder or the link can supply a look

void NoteGameThread() noexcept;     // game thread: remember its id (the detour logs whether it runs there)
// Game thread, every tick: releases one texture of ours nobody used for 5 s (not during a load). textures_free=1.
void FreeTick(double now_ms, bool world_ready) noexcept;

// ---------------------------------------------------------------- looks
// One built texture: the slot of tex_slots it replaces, the blob (tex_codec layout) and its SHA-256.
struct Blob {
    int slot = -1;
    std::uint8_t id[32] = {};
    std::vector<std::uint8_t> bytes;
};
// A look: the textures to bind on her suit (empty = her own look). `id` = SHA-256 of the sorted {slot, blob id} list.
struct Look {
    std::uint8_t id[32] = {};
    std::vector<int> slots;
    std::vector<std::uint8_t> blob_ids;  // 32 bytes per slot, same order
    int colour = -1;
    bool Empty() const { return slots.empty(); }
};

// The blob store (built by texture_looks, received by texture_share). Any thread. RAM holds at most kStoreRamMax bytes of
// blobs (least recently used out first); a blob whose bytes were let go of is read again from its disk file (`path`: the
// built cache or the partner cache, validated against its id) when it is needed. A blob with no file stays in RAM.
inline constexpr std::size_t kStoreRamMax = 64u << 20;
bool BlobBytes(const std::uint8_t id[32], std::vector<std::uint8_t>* out) noexcept;
bool HaveBlob(const std::uint8_t id[32]) noexcept;  // in RAM, or its file is there
void StoreBlob(const std::uint8_t id[32], int slot, std::vector<std::uint8_t>&& bytes, const std::wstring& path = {}) noexcept;
void StoreReceived(const std::uint8_t id[32], int slot, std::vector<std::uint8_t>&& bytes, const std::wstring& path = {}) noexcept;
// tex 7 (host): lets go of these blobs' RAM copies (the next use reads their disk files). Returns how many.
unsigned DropRam(const std::vector<const std::uint8_t*>& ids) noexcept;
std::size_t StoreRamBytes() noexcept;

// ---------------------------------------------------------------- binding (game thread)
struct Dress {
    std::uintptr_t e = 0;
    std::uintptr_t drawable = 0;
    std::uint8_t applied[32] = {};   // look id bound now (all zero = none)
    bool bound = false;
    std::uint8_t pending[32] = {};
    bool pending_set = false;
    double pending_ms = 0, next_ms = 0;
    std::uint64_t handles[32] = {};
    unsigned nh = 0;
    int reg[24] = {};                // registry indices of our filtered entries (suit and boots: up to 11 per mesh)
    unsigned nreg = 0;
    unsigned applies = 0, undos = 0;
    const char* who = "body";
    double bound_ms = 0;
    bool waiting_rebuild = false;
    unsigned refused_logged = 0;
    void* bound_clones[8] = {};  // our textures this body's bound look uses (kept alive while bound)
    unsigned nbound = 0;
};
// Brings `e` (a live fragile_suit body of ours) towards `look` (empty = her own look): makes the look's textures (one per
// call), then binds them through the filtered override. Returns true while ftex owns the body's overrides (bound or
// binding); false when the look is empty and nothing of ours is bound (the caller may use the v0.23 tint instead).
bool Apply(Dress& d, std::uintptr_t e, const Look& look, double now_ms) noexcept;
// While the wanted look is not known yet (the worker rebuilds it): whatever is bound on `e` stays, its textures marked used.
void Keep(Dress& d, std::uintptr_t e, double now_ms) noexcept;
void Undo(Dress& d, const char* why) noexcept;
void Forget(Dress& d) noexcept;
const char* Describe(const Dress& d, char* buf, std::size_t cap) noexcept;

// Debug (tex <mode>): 0 status, 1 rescan, 2 probe dump on the next rebuild, 3 force colour, 4 off/on, 5 filter mode,
// 7 resend, 8 template check, 9 test pattern, 10 force the boot type, 11 the metal variant of the ready-made gold / silver,
// 12 (host) how a guest's ready-made colour is made (0 here, 1 always sent over the link, 2 pretend the originals differ). `e_self` / `e_slot`: our live fragile_suit bodies (0 = none).
bool DebugCommand(int mode, double a, double b, std::uintptr_t e_self, std::uintptr_t e_slot, char* msg, std::size_t cap) noexcept;
int ForcedColour() noexcept;      // tex 3 (-1 = follow)
int ForcedBoots() noexcept;       // tex 10: the boot type her boots pictures follow (-1 = our Sam's)
bool PatternOn() noexcept;        // tex 9

// Counters for the status line.
struct Stats {
    unsigned rebuilds = 0, kept = 0, dropped = 0, faults = 0, made = 0, make_failed = 0, built = 0;
};
Stats GetStats() noexcept;

}  // namespace st::ftex
