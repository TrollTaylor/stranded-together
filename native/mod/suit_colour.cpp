#include "suit_colour.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdio>
#include <cstring>

#include "automation.hpp"
#include "coop_session.hpp"
#include "log.hpp"
#include "partner_source.hpp"
#include "suit_colour_math.hpp"

namespace st::suitcol {
namespace {

namespace mem = partner::mem;
using mem::Q;
using mem::Rd;

// ---------------------------------------------------------------- ds.exe (RVAs at base 0x140000000)
// The texture-set override of ApplyArtPartsVariation (0x1422BC100 -> 0x1422BAE20 -> 0x1422BBB80, Sam's silver / gold
// suit): 0x1422BBB80 takes the drawable as *(*(*(entity+0xB8)+0x40)+0x28) and, per mesh, calls the core
// 0x14237CA20(drawable, String* node, TextureSet*, Array<u64>* handles, int mesh); 0x142378C20(drawable, 1) after.
constexpr std::uintptr_t kApvSet = 0x22BBB80;
constexpr std::uintptr_t kApvChain0 = 0x22BBD8D;   // mov rax,[r13+0xB8]
constexpr std::uintptr_t kApvChain1 = 0x22BBD9C;   // mov rax,[rax+0x40]
constexpr std::uintptr_t kApvChain2 = 0x22BBDA4;   // mov r14,[rax+0x28]
constexpr std::uintptr_t kApvRefreshCall = 0x22BC0BB;  // mov rcx,[rdi+0x28]; mov dl,1; call 0x142378C20
constexpr std::uintptr_t kCore = 0x237CA20;
constexpr std::uintptr_t kUndo = 0x237A960;     // (drawable, u64* handle): the override off, the original binding back
constexpr std::uintptr_t kRefresh = 0x2378C20;  // (drawable, bool): bindings dirty, mesh caches reset
constexpr std::uintptr_t kMeshTable = 0x2374D50;
constexpr std::uintptr_t kBindTex = 0x1DF5170;  // (instance, &handle, Texture*, swizzle): stores Texture* plain (+0x30)
constexpr std::uintptr_t kEntryOf = 0x1D25F60;  // (TextureSet*, type): the entry whose packing nibbles name the type
// The game's texture loader: 0x141D24460(HwTexture** out, reader, u32* dims) reads the 0x20-byte header with
// reader->vtbl[+0x10], the u32 data size (0x141909CC0), creates the hardware texture with the factory
// (*0x144F6D4F8, class vtable 0x143B9B8C8, slot +0x18 = 0x14197BE60: a 2D texture of class vtable 0x143B9B488) and
// lets it read its data (slot +0x10 = 0x14197CAF0: three u32, one u64 (0x141909DF0), the embedded mips
// (0x141980830): only reader->vtbl[+0x10] and the reader's +0x0C byte-order flag); reader->vtbl[+0x28] tells the
// position; reader->vtbl[+0x18] skips (only when the texture cache already holds the GUID).
constexpr std::uintptr_t kLoader = 0x1D24460;
constexpr std::uintptr_t kTexReadBinary = 0x1D242C0;  // Texture's handler: Texture+0x20 = loader(..., Texture+0x28)
constexpr std::uintptr_t kLoaderRd32Call = 0x1D24548;
constexpr std::uintptr_t kRd32 = 0x1909CC0, kRd64 = 0x1909DF0, kEmbedded = 0x1980830;
constexpr std::uintptr_t kHwRead = 0x197CAF0;
constexpr std::uintptr_t kHwReadRd64Call = 0x197CC76, kHwReadEmbeddedCall = 0x197D0B1;
constexpr std::uintptr_t kCreate2D = 0x197BE60;
constexpr std::uintptr_t kFactory = 0x4F6D4F8;       // the hardware-resource factory (a pointer)
constexpr std::uintptr_t kFactoryVtbl = 0x3B9B8C8;
constexpr std::uintptr_t kHwTexVtbl = 0x3B9B488;
constexpr std::uintptr_t kTexVtbl = 0x3BC7C50;       // Texture (RTTIRefObject, 0x30 bytes)
constexpr std::uintptr_t kTexGetter = 0x1B36BC0;     // lea rax, Texture RTTI; ret
constexpr std::uintptr_t kTexRtti = 0x4A50BA0;
constexpr std::uintptr_t kEmptyString = 0x4562CA0;   // the game's empty String header (refs, hash, length 0, then the text)

// Drawable fields (0x14237CA20 / 0x142374D50 / 0x142378C20).
constexpr std::uintptr_t kDrMeshes = 0x98;       // -> +0x40 i32 count, +0x48 entries of 0x1B0
constexpr std::uintptr_t kDrOverrides = 0x1E8;   // i32 count, +0x1F0 sorted {i32 mesh, ptr table}
constexpr std::uintptr_t kMeshEntry = 0x1B0;

// ---------------------------------------------------------------- state
struct State {
    bool checked = false, ok = false, off = false, cfg = true;
    char why[200] = "not checked";
    long long test_mask = -1;
    unsigned test_channels = 0;
    int test_metal = 0, test_rough = -1;
} g;

bool Check(const char* name, std::uintptr_t rva, const char* sig) {
    if (mem::CodeMatches(mem::Base() + rva, sig)) return true;
    std::snprintf(g.why, sizeof(g.why), "%s at ds.exe+0x%llX does not match", name, static_cast<unsigned long long>(rva));
    return false;
}

void Fault(const char* what) {
    g.off = true;
    Log("[suit] %s FAULTED: Fragile's suit colours off for this session", what);
}

const char* TypeOf(std::uintptr_t obj) {
    const char* n = obj ? mem::RttiName(mem::RttiOf(obj)) : nullptr;
    return n ? n : "?";
}

// ---------------------------------------------------------------- the binary reader the loader reads through
// Layout the game's readers share as far as the loader uses it: vtable, +0x0C byte order (0 = little endian, read by
// 0x141909CC0 / 0x141909DF0), +0x10 a state the stream readers keep at 2 (ok). Our own fields follow.
struct Reader {
    void* const* vtbl;
    std::uint32_t r08;
    std::uint32_t big_endian;
    std::uint32_t state;
    std::uint32_t r14;
    std::uintptr_t spare[6];
    const std::uint8_t* data;
    std::uint32_t size, pos;
    bool overrun;
    unsigned odd;  // bit per unexpected vtable slot called
};
using Slot = std::uintptr_t (*)(Reader*, std::uintptr_t, std::uintptr_t, std::uintptr_t);
std::uintptr_t RDtor(Reader* r, std::uintptr_t, std::uintptr_t, std::uintptr_t) { return reinterpret_cast<std::uintptr_t>(r); }
std::uintptr_t RRead(Reader* r, std::uintptr_t dst, std::uintptr_t n, std::uintptr_t) {
    const std::uint32_t want = static_cast<std::uint32_t>(n);
    std::uint8_t* out = reinterpret_cast<std::uint8_t*>(dst);
    const std::uint32_t have = r->pos < r->size ? r->size - r->pos : 0;
    const std::uint32_t k = want < have ? want : have;
    if (k) std::memcpy(out, r->data + r->pos, k);
    if (k < want) {
        std::memset(out + k, 0, want - k);
        r->overrun = true;
    }
    r->pos += k;
    return want;
}
std::uintptr_t RSkip(Reader* r, std::uintptr_t n, std::uintptr_t, std::uintptr_t) {
    const std::int32_t k = static_cast<std::int32_t>(n);
    const std::int64_t p = static_cast<std::int64_t>(r->pos) + k;
    r->pos = p < 0 ? 0 : p > r->size ? r->size : static_cast<std::uint32_t>(p);
    return 1;
}
std::uintptr_t RTell(Reader* r, std::uintptr_t, std::uintptr_t, std::uintptr_t) { return r->pos; }
template <int N>
std::uintptr_t ROdd(Reader* r, std::uintptr_t, std::uintptr_t, std::uintptr_t) {
    r->odd |= 1u << N;
    return 0;
}
Slot const kReaderVtbl[16] = {&RDtor,    &ROdd<1>,  &RRead,    &RSkip,    &ROdd<4>,  &RTell,    &ROdd<6>,  &ROdd<7>,
                              &ROdd<8>,  &ROdd<9>,  &ROdd<10>, &ROdd<11>, &ROdd<12>, &ROdd<13>, &ROdd<14>, &ROdd<15>};

// ---------------------------------------------------------------- our constant textures
// A game Texture object (vtable, ObjectUUID +0x08, refs +0x18, HwTexture* +0x20, dims +0x28) of our own memory: its
// reference count is held high (no Release ever frees it) and it lives for the session (the bindings keep the pointer).
struct TexObj {
    alignas(8) std::uint8_t b[0x30];
};
struct PoolTex {
    std::uint32_t key = 0;  // rgb + srgb + 1 (0 = empty)
    TexObj obj{};
    std::uintptr_t hw = 0;
};
PoolTex g_pool[48];
unsigned g_made = 0, g_make_failed = 0;

std::uintptr_t CallLoader(std::uintptr_t* hw_out, Reader* r, std::uint32_t* dims) {
    __try {
        reinterpret_cast<std::uintptr_t* (*)(std::uintptr_t*, Reader*, std::uint32_t*)>(mem::Base() + kLoader)(hw_out, r, dims);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// A 1x1 RGBA8 texture holding `c` (alpha 255): the game's own serialized form of its 1x1 constants (the 0x20-byte
// header: 2D, 1x1, 1 mip, format 0x0C RGBA8, +0x0B the colour-space flag, +0x0D 0xA9 as the game's constants, a GUID of
// ours; the data size 0x18; embedded 4 bytes, nothing streamed; the pixel), read by the game's loader.
std::uintptr_t MakeTexture(Rgb c, bool srgb) {
    const std::uint32_t key = (static_cast<std::uint32_t>(c.r) << 24 | static_cast<std::uint32_t>(c.g) << 16 | static_cast<std::uint32_t>(c.b) << 8 |
                               (srgb ? 2u : 0u)) + 1u;
    PoolTex* free_slot = nullptr;
    for (PoolTex& p : g_pool) {
        if (p.key == key && p.hw) return reinterpret_cast<std::uintptr_t>(&p.obj);
        if (!p.key && !free_slot) free_slot = &p;
    }
    if (!free_slot) {
        Log("[suit] constant texture pool full");
        return 0;
    }
    // The live factory must be the class whose 2D texture reads its data the way analysed above.
    const std::uintptr_t b = mem::Base();
    const std::uintptr_t fac = Q(b + kFactory);
    if (!fac || Q(fac) != b + kFactoryVtbl) {
        Log("[suit] the texture factory %p (vtable %p) is not the expected class: no texture made", reinterpret_cast<void*>(fac),
            reinterpret_cast<void*>(fac ? Q(fac) : 0));
        ++g_make_failed;
        return 0;
    }
    std::uint8_t blob[0x20 + 4 + 0x18] = {};
    std::uint8_t* h = blob;
    h[2] = 1;                // width 1 (u16 at +2)
    h[4] = 1;                // height 1 (u16 at +4)
    h[8] = 1;                // mips
    h[9] = 0x0C;             // RGBA8
    h[0x0B] = srgb ? 1 : 0;  // colour space (the game's 1x1 Color constants: 1)
    h[0x0D] = 0xA9;          // as the game's 1x1 constants
    h[0x0E] = 0xFF;
    // GUID: "STSC" + the colour + the colour space + a fixed tail (ours only; the texture cache keys on it)
    const std::uint8_t guid[16] = {0x53, 0x54, 0x53, 0x43, c.r, c.g, c.b, static_cast<std::uint8_t>(srgb ? 1 : 0),
                                   0x9E, 0x37, 0x79, 0xB9, 0x7F, 0x4A, 0x7C, 0x15};
    std::memcpy(h + 0x10, guid, 16);
    std::uint8_t* p = blob + 0x20;
    p[0] = 0x18;      // data size
    p[4] = 4;         // embedded bytes
    // +8 streamed 0, +0x0C first embedded mip 0, +0x10 u64 0
    p[0x18] = c.r, p[0x19] = c.g, p[0x1A] = c.b, p[0x1B] = 0xFF;
    Reader r{};
    r.vtbl = reinterpret_cast<void* const*>(kReaderVtbl);
    r.state = 2;
    r.data = blob;
    r.size = sizeof(blob);
    TexObj& o = free_slot->obj;
    std::memset(o.b, 0, sizeof(o.b));
    std::uintptr_t hw = 0;
    if (!CallLoader(&hw, &r, reinterpret_cast<std::uint32_t*>(o.b + 0x28))) {
        Fault("the game's texture loader");
        return 0;
    }
    if (!hw || r.overrun || r.odd || r.pos != r.size) {
        Log("[suit] texture %02x%02x%02x%s: loader gave %p (read %u of %u bytes%s, unexpected reader calls 0x%X): not used", c.r, c.g, c.b,
            srgb ? " sRGB" : "", reinterpret_cast<void*>(hw), r.pos, r.size, r.overrun ? ", OVERRUN" : "", r.odd);
        ++g_make_failed;
        if (hw) free_slot->key = 0xFFFFFFFFu;  // keep the slot (we own a reference we cannot safely drop here)
        return 0;
    }
    const std::uintptr_t vt = mem::Base() + kTexVtbl;
    std::memcpy(o.b, &vt, 8);
    std::memcpy(o.b + 0x08, guid, 16);
    const std::uint32_t refs = 0x40000000u;
    std::memcpy(o.b + 0x18, &refs, 4);
    std::memcpy(o.b + 0x20, &hw, 8);
    free_slot->key = key;
    free_slot->hw = hw;
    ++g_made;
    std::uintptr_t hwvt = 0;
    Rd(hw, hwvt);
    Log("[suit] constant texture %02x%02x%02x%s made by the game's loader: Texture %p, hardware texture %p (vtable ds.exe+0x%llX)%s", c.r, c.g, c.b,
        srgb ? " (sRGB)" : "", static_cast<void*>(&o), reinterpret_cast<void*>(hw), static_cast<unsigned long long>(hwvt - mem::Base()),
        hwvt == mem::Base() + kHwTexVtbl ? "" : " (UNEXPECTED class)");
    return reinterpret_cast<std::uintptr_t>(&o);
}

// ---------------------------------------------------------------- the TextureSet of ours (only what 0x14237CA20 reads)
// +0x20 i32 entry count, +0x28 entries (0x18 each: 4 packing bytes (low nibble: channel type, bits 4-6: the shader
// component, bit 7: one channel broadcast), +0x08 Texture*, +0x10 byte (&3 == 1: use the packing as the swizzle));
// +0x38 i32 desc count, +0x40 descs (0x40 each: +0 type, +0x08 String path, +0x10 Active).
struct SetEntry {
    std::uint8_t pack[4];
    std::uint32_t pad;
    std::uintptr_t tex;
    std::uint8_t flags;
    std::uint8_t pad2[7];
};
static_assert(sizeof(SetEntry) == 0x18, "TextureSet entry");
struct SetDesc {
    std::uint32_t type;
    std::uint32_t pad;
    std::uintptr_t path;
    std::uint8_t active;
    std::uint8_t gamma;
    std::uint8_t rest[0x40 - 0x12];
};
static_assert(sizeof(SetDesc) == 0x40, "TextureSet desc");
struct FakeSet {
    std::uint8_t head[0x20];
    std::int32_t entries_n, entries_cap;
    SetEntry* entries;
    std::uint8_t mid[0x08];
    std::int32_t descs_n, descs_cap;
    SetDesc* descs;
    std::uint8_t tail[0x10];
};
static_assert(offsetof(FakeSet, entries) == 0x28 && offsetof(FakeSet, descs) == 0x40, "TextureSet layout");

// The binding name's node, as a game String (refs held high: never freed): "inSampler_Main_TextureSet".
struct NodeString {
    std::uint32_t refs = 0x40000000u;
    std::uint32_t crc = 0xFFFFFFFFu;
    std::uint32_t len = 25;
    std::uint32_t pad = 0;
    char text[32] = "inSampler_Main_TextureSet";
} g_node_str;
const char* g_node = g_node_str.text;

// The game's array the core appends handles to ({i32 count, i32 cap, u64* data}, grown by the game's allocator): one,
// reused (emptied before each call, the handles copied out).
struct GameArray {
    std::int32_t n = 0, cap = 0;
    std::uint64_t* data = nullptr;
} g_arr;

bool CallCore(std::uintptr_t drawable, FakeSet* set, int mesh) {
    __try {
        reinterpret_cast<void (*)(std::uintptr_t, const char**, FakeSet*, GameArray*, int)>(mem::Base() + kCore)(drawable, &g_node, set, &g_arr, mesh);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool CallUndo(std::uintptr_t drawable, std::uint64_t* h) {
    __try {
        reinterpret_cast<void (*)(std::uintptr_t, std::uint64_t*)>(mem::Base() + kUndo)(drawable, h);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool CallRefresh(std::uintptr_t drawable) {
    __try {
        reinterpret_cast<void (*)(std::uintptr_t, char)>(mem::Base() + kRefresh)(drawable, 1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The body's model drawable (the chain 0x1422BBB80 reads), or 0. `meshes` = its mesh count.
std::uintptr_t Drawable(std::uintptr_t e, int* meshes) {
    *meshes = 0;
    const std::uintptr_t a = Q(e + 0xB8);
    const std::uintptr_t bb = a ? Q(a + 0x40) : 0;
    const std::uintptr_t d = bb ? Q(bb + 0x28) : 0;
    if (!d) return 0;
    const std::uintptr_t m = Q(d + kDrMeshes);
    std::int32_t n = 0;
    if (!m || !Rd(m + 0x40, n) || n <= 0 || n > 256 || !Q(m + 0x48)) return 0;
    *meshes = n;
    return d;
}

void GuidText(const std::uint8_t* g16, char* out, std::size_t cap) {
    std::snprintf(out, cap, "%02x%02x%02x%02x-%02x%02x-%02x%02x", g16[3], g16[2], g16[1], g16[0], g16[5], g16[4], g16[7], g16[6]);
}

// Which meshes the product binds: every mesh (her suit model's meshes; the head and hair are other models).
bool MeshWanted(int i) {
    if (g.test_mask == -1) return true;
    return g.test_mask >= 0 && i < 63 && ((g.test_mask >> i) & 1);
}

}  // namespace

bool Available() noexcept {
    if (g.checked) return g.ok && !g.off;
    g.checked = true;
    const std::uintptr_t b = mem::Base();
    bool ok = Check("texture-set override (ApplyArtPartsVariation part)", kApvSet, "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 68 E0 FF FF") &&
              Check("drawable chain +0xB8", kApvChain0, "49 8B 85 B8 00 00 00") && Check("drawable chain +0x40", kApvChain1, "48 8B 40 40") &&
              Check("drawable chain +0x28", kApvChain2, "4C 8B 70 28") && Check("override refresh call", kApvRefreshCall, "48 8B 4F 28 B2 01 E8") &&
              Check("texture-set override core", kCore, "40 55 53 56 57 41 54 41 55 41 56 48 8D 6C 24 E1 48 81 EC C0 00 00 00") &&
              Check("override undo", kUndo, "40 53 41 56 41 57 48 83 EC 40 44 0F BF 4A 02 4C 8D B1 E8 01 00 00") &&
              Check("override refresh", kRefresh, "40 53 56 48 83 EC 28 44 8B 81 A8 01 00 00 33 DB 83 89 A4 01 00 00 10") &&
              Check("mesh override table", kMeshTable, "40 53 57 41 56 48 83 EC 30 4C 8D B1 E8 01 00 00") &&
              Check("texture bind", kBindTex, "44 0F B7 1A 45 33 D2 66 45 85 DB 78 1B 8B 51 10") &&
              Check("texture-set entry lookup", kEntryOf, "48 63 41 20 45 33 C9 85 C0 7E 4F 4C 8B 59 28") &&
              Check("texture loader", kLoader, "40 55 53 56 57 41 54 41 56 41 57 48 8D 6C 24 D9 48 81 EC E0 00 00 00") &&
              Check("Texture read handler", kTexReadBinary, "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 48 8B 52 08") &&
              Check("loader size read call", kLoaderRd32Call, "E8") &&
              Check("u32 read", kRd32, "48 89 5C 24 18 57 48 83 EC 30 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 28") &&
              Check("u64 read", kRd64, "48 89 5C 24 18 57 48 83 EC 40 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 30") &&
              Check("embedded mip read", kEmbedded, "44 89 4C 24 20 44 89 44 24 18 48 89 4C 24 08 53 56 41 55 48 83 EC 60 8B 41 38") &&
              Check("2D texture data read", kHwRead, "48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 D9 48 81 EC E0 00 00 00") &&
              Check("2D texture u64 read call", kHwReadRd64Call, "E8") && Check("2D texture embedded read call", kHwReadEmbeddedCall, "E8") &&
              Check("2D texture create", kCreate2D, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 20") &&
              Check("Texture RTTI getter", kTexGetter, "48 8D 05 ?? ?? ?? ?? C3");
    if (ok && (mem::RipTarget(b + kApvRefreshCall + 6, 1, 5) != b + kRefresh || mem::RipTarget(b + kLoaderRd32Call, 1, 5) != b + kRd32 ||
               mem::RipTarget(b + kHwReadRd64Call, 1, 5) != b + kRd64 || mem::RipTarget(b + kHwReadEmbeddedCall, 1, 5) != b + kEmbedded ||
               mem::RipTarget(b + kTexGetter, 3, 7) != b + kTexRtti)) {
        std::snprintf(g.why, sizeof(g.why), "the override / loader call sites do not lead where expected");
        ok = false;
    }
    std::uintptr_t s0 = 0, s1 = 0, s2 = 0;
    if (ok && (!Rd(b + kTexVtbl, s0) || s0 != b + kTexGetter || !Rd(b + kHwTexVtbl + 0x10, s1) || s1 != b + kHwRead ||
               !Rd(b + kFactoryVtbl + 0x18, s2) || s2 != b + kCreate2D)) {
        std::snprintf(g.why, sizeof(g.why), "the Texture / 2D texture / factory vtables differ");
        ok = false;
    }
    const char* tn = ok ? mem::RttiName(b + kTexRtti) : nullptr;
    if (ok && (!tn || std::strcmp(tn, "Texture") != 0)) {
        std::snprintf(g.why, sizeof(g.why), "the Texture RTTI is not named Texture");
        ok = false;
    }
    // The empty String: length 0 at +0x08 and an empty text at +0x10 (its refs / hash words change at run time).
    std::uint8_t es[17] = {};
    std::uint32_t es_len = 1;
    if (ok && (!mem::Read(b + kEmptyString, es, sizeof(es)) || (std::memcpy(&es_len, es + 8, 4), es_len != 0) || es[16] != 0)) {
        std::snprintf(g.why, sizeof(g.why), "the game's empty String differs (length %u)", es_len);
        ok = false;
    }
    if (ok) std::snprintf(g.why, sizeof(g.why), "ok");
    Log("[suit] Fragile's suit colours %s%s", ok ? "available (texture-set override, undo, the game's texture loader and its 2D texture verified)"
                                                 : "unavailable: ",
        ok ? "" : g.why);
    g.ok = ok;
    return ok;
}

const char* Why() noexcept { return g.off ? "a game call faulted earlier" : g.why; }

void Configure(bool on) noexcept {
    g.cfg = on;
    Log("[suit] Fragile's porter suit follows the suit colour (session.ini fragile_suit_colour=%d)", on ? 1 : 0);
}

bool Enabled() noexcept { return g.cfg && Available(); }

namespace {
SRWLOCK g_pick_lock = SRWLOCK_INIT;
GuestPick g_pick;
std::uint8_t g_pick_last_out = 0xFF;  // what Fragile wore in the last ready world
bool g_pick_on = false;
std::wstring g_pick_path;
void SavePick(std::uint8_t c) noexcept {
    if (g_pick_path.empty()) return;
    const HANDLE h = CreateFileW(g_pick_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        Log("[suit] the guest's suit colour pick %u could not be remembered (error %lu)", c, GetLastError());
        return;
    }
    char b[16];
    const int n = std::snprintf(b, sizeof(b), "%u\r\n", c);
    DWORD w = 0;
    WriteFile(h, b, static_cast<DWORD>(n), &w, nullptr);
    CloseHandle(h);
    Log("[suit] the guest picked suit colour %u: remembered for Fragile across the host's loads", c);
}
}  // namespace

void ConfigureGuestPick(bool on, const std::wstring& dir) noexcept {
    AcquireSRWLockExclusive(&g_pick_lock);
    g_pick_on = on;
    g_pick = GuestPick{};
    g_pick_last_out = 0xFF;
    g_pick_path.clear();
    if (on) {
        const std::wstring d = automation::Enabled() ? automation::OutDir() : dir;
        if (!d.empty()) g_pick_path = d + L"\\fragile_suit_colour.txt";
        char b[16] = {};
        DWORD got = 0;
        const HANDLE h = g_pick_path.empty() ? INVALID_HANDLE_VALUE
                                             : CreateFileW(g_pick_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            ReadFile(h, b, sizeof(b) - 1, &got, nullptr);
            CloseHandle(h);
            unsigned v = 0xFF;
            if (got && std::sscanf(b, "%u", &v) == 1 && v <= kGold) g_pick.saved = static_cast<std::uint8_t>(v);
        }
    }
    const std::uint8_t saved = g_pick.saved;
    ReleaseSRWLockExclusive(&g_pick_lock);
    if (on) {
        if (saved <= kGold) Log("[suit] guest: Fragile wears the remembered pick %u until the guest picks again", saved);
        else Log("[suit] guest: no remembered suit colour pick yet (Fragile wears Sam's colour; a pick is remembered)");
    }
}

std::uint8_t GuestColour(std::uint8_t cur) noexcept {
    std::uint32_t world = 0;
    const bool ready = coop::WorldReady(&world);
    AcquireSRWLockExclusive(&g_pick_lock);
    if (!g_pick_on) {
        ReleaseSRWLockExclusive(&g_pick_lock);
        return cur;
    }
    const bool new_world = ready && world != g_pick.world;
    const bool changed = ready && g_pick.See(world, cur);
    // between worlds (a load: her Sam's byte is already the host's again) Fragile keeps what she wore
    const std::uint8_t out = ready ? g_pick.Effective(cur) : g_pick_last_out <= kGold ? g_pick_last_out : cur;
    if (ready) g_pick_last_out = out;
    const std::uint8_t saved = g_pick.saved;
    ReleaseSRWLockExclusive(&g_pick_lock);
    if (changed) SavePick(saved);
    if (new_world && saved <= kGold && saved != cur)
        Log("[suit] guest: world #%u loaded with Sam's suit colour %u: Fragile keeps the guest's remembered pick %u", world, cur, saved);
    return out;
}

void SetTest(long long mesh_mask, unsigned channels) noexcept {
    g.test_mask = mesh_mask;
    g.test_channels = channels & 7u;
    Log("[suit] TEST: meshes %s (mask %lld), channels %s%u", mesh_mask == -1 ? "the product's choice" : mesh_mask == -2 ? "the whole model (mesh -1)" : "by mask",
        mesh_mask, channels ? "" : "the product's choice / ", channels & 7u);
}

void SetTestMetal(int variant, int roughness) noexcept {
    g.test_metal = variant >= 0 && variant < kMetalVariants ? variant : 0;
    g.test_rough = roughness >= 0 && roughness <= 255 ? roughness : -1;
    Log("[suit] TEST: metal variant %d, roughness %d (-1: the variant's)", g.test_metal, g.test_rough);
}

void Forget(Dress& d) noexcept { d = Dress{}; }

void Undo(Dress& d, const char* why) noexcept {
    if (!d.nh) {
        d.applied = 0;
        return;
    }
    int meshes = 0;
    const std::uintptr_t dr = d.e ? Drawable(d.e, &meshes) : 0;
    if (!dr || dr != d.drawable) {
        Log("[suit] body %p: %u override(s) not undone (%s): the drawable %p is gone or changed (%p)", reinterpret_cast<void*>(d.e), d.nh, why,
            reinterpret_cast<void*>(d.drawable), reinterpret_cast<void*>(dr));
        d.nh = 0;
        d.applied = 0;
        return;
    }
    for (unsigned i = 0; i < d.nh; ++i) {
        if (!CallUndo(dr, &d.handles[i])) return Fault("the override undo");
    }
    if (!CallRefresh(dr)) return Fault("the override refresh");
    ++d.undos;
    Log("[suit] body %p: colour %u taken off (%u override(s) undone, %s): her own suit again", reinterpret_cast<void*>(d.e), d.applied, d.nh, why);
    d.nh = 0;
    d.applied = 0;
}

void Apply(Dress& d, std::uintptr_t e, std::uint8_t colour, double now_ms) noexcept {
    if (!e) return;
    if (d.e != e) {
        Forget(d);
        d.e = e;
    }
    if (!Available()) return;
    if (!g.cfg) colour = 0;
    if (colour > kGold) colour = 0;
    if (now_ms < d.next_ms) return;
    d.next_ms = now_ms + 250;
    if (colour != d.pending) {
        d.pending = colour;
        d.pending_ms = now_ms;
    }
    int meshes = 0;
    const std::uintptr_t dr = Drawable(e, &meshes);
    if (d.nh && dr != d.drawable) {
        Log("[suit] body %p: its drawable changed (%p -> %p): the overrides went with the old one", reinterpret_cast<void*>(e),
            reinterpret_cast<void*>(d.drawable), reinterpret_cast<void*>(dr));
        d.nh = 0;
        d.applied = 0;
    }
    if (colour == d.applied || now_ms - d.pending_ms < 500) return;
    if (d.nh) return Undo(d, "colour change");
    d.applied = 0;
    if (colour == 0) return;
    if (!dr) {
        if (d.refused_logged != colour) {
            d.refused_logged = colour;
            Log("[suit] body %p: colour %u not applied: no model drawable (*(*(*(e+0xB8)+0x40)+0x28))", reinterpret_cast<void*>(e), colour);
        }
        return;
    }
    Look look = LookFor(colour, g.test_metal);
    if (g.test_rough >= 0 && (look.channels & kChRoughness)) look.roughness = static_cast<std::uint8_t>(g.test_rough);
    unsigned ch = g.test_channels ? g.test_channels : look.channels;
    if (ch & kChReflectance && !(look.channels & kChReflectance)) ch &= ~kChReflectance;  // only the metals carry a reflectance
    if (ch & kChRoughness && !(look.channels & kChRoughness)) ch &= ~kChRoughness;
    SetEntry entries[3] = {};
    SetDesc descs[3] = {};
    int n = 0;
    const std::uintptr_t empty_path = mem::Base() + kEmptyString + 0x10;
    auto add = [&](std::uint32_t type, std::uintptr_t tex, std::uint8_t p0, std::uint8_t p1, std::uint8_t p2) {
        SetEntry& en = entries[n];
        en.pack[0] = p0, en.pack[1] = p1, en.pack[2] = p2, en.pack[3] = 0;
        en.tex = tex;
        en.flags = 1;
        SetDesc& ds = descs[n];
        ds.type = type;
        ds.path = empty_path;
        ds.active = 1;
        ds.gamma = type == kTypeColor ? 1 : 0;
        ++n;
    };
    if (ch & kChColor) {
        const std::uintptr_t t = MakeTexture(look.color, true);
        if (!t) return;
        add(kTypeColor, t, 0x01, 0x11, 0x21);  // RGB -> the shader's Color xyz
    }
    if (ch & kChReflectance) {
        const std::uintptr_t t = MakeTexture(look.reflectance, false);
        if (!t) return;
        add(kTypeReflectance, t, 0x04, 0x14, 0x24);
    }
    if (ch & kChRoughness) {
        const std::uintptr_t t = MakeTexture(Rgb{look.roughness, look.roughness, look.roughness}, false);
        if (!t) return;
        add(kTypeRoughness, t, 0x86, 0x00, 0x00);  // one channel: R broadcast (as her own set's Roughness in the Color alpha)
    }
    if (g.off || !n) return;
    FakeSet set{};
    set.entries_n = set.entries_cap = n;
    set.entries = entries;
    set.descs_n = set.descs_cap = n;
    set.descs = descs;
    d.nh = 0;
    d.drawable = dr;
    unsigned calls = 0;
    auto bind = [&](int mesh) -> bool {
        g_arr.n = 0;
        if (!CallCore(dr, &set, mesh)) {
            Fault("the texture-set override core");
            return false;
        }
        ++calls;
        for (int i = 0; i < g_arr.n && g_arr.data; ++i) {
            if (d.nh < sizeof(d.handles) / sizeof(d.handles[0])) d.handles[d.nh++] = g_arr.data[i];
        }
        g_arr.n = 0;
        return true;
    };
    if (g.test_mask == -2) {
        if (!bind(-1)) return;
    } else {
        for (int i = 0; i < meshes; ++i) {
            if (MeshWanted(i) && !bind(i)) return;
        }
    }
    if (!CallRefresh(dr)) return Fault("the override refresh");
    ++d.applies;
    d.binds += d.nh;
    d.applied = colour;
    Log("[suit] body %p: colour %u on (#%u: %s%s%s; albedo %02x%02x%02x%s, reflectance %02x%02x%02x roughness %u, metal variant %d): %u core call(s) over %d mesh(es), %u override(s), drawable %p",
        reinterpret_cast<void*>(e), colour, d.applies, ch & kChColor ? "Color" : "", ch & kChReflectance ? " Reflectance" : "", ch & kChRoughness ? " Roughness" : "",
        look.color.r, look.color.g, look.color.b, colour >= kSilver ? (colour == kGold ? ", gold" : ", silver") : "", look.reflectance.r,
        look.reflectance.g, look.reflectance.b, look.roughness, g.test_metal, calls, meshes, d.nh,
        reinterpret_cast<void*>(dr));
}

const char* Describe(const Dress& d, char* buf, std::size_t cap) noexcept {
    std::snprintf(buf, cap, "body %p colour %u (wanted %u) overrides %u applies %u undos %u; textures made %u (failed %u)%s",
                  reinterpret_cast<void*>(d.e), d.applied, d.pending, d.nh, d.applies, d.undos, g_made, g_make_failed,
                  g.cfg ? "" : " [fragile_suit_colour=0]");
    return buf;
}

void LogBody(std::uintptr_t e, const char* what) noexcept {
    const std::uintptr_t b = mem::Base();
    const std::uintptr_t a = Q(e + 0xB8);
    const std::uintptr_t bb = a ? Q(a + 0x40) : 0;
    const std::uintptr_t dr = bb ? Q(bb + 0x28) : 0;
    auto vt = [&](std::uintptr_t o) -> unsigned long long {
        const std::uintptr_t v = o ? Q(o) : 0;
        return v > b ? static_cast<unsigned long long>(v - b) : 0ull;
    };
    Log("[suit] %s %p: +0xB8 %p (%s, vt +0x%llX) +0x40 %p (%s, vt +0x%llX) +0x28 drawable %p (vt +0x%llX)", what, reinterpret_cast<void*>(e),
        reinterpret_cast<void*>(a), TypeOf(a), vt(a), reinterpret_cast<void*>(bb), TypeOf(bb), vt(bb), reinterpret_cast<void*>(dr), vt(dr));
    if (!dr) return;
    const std::uintptr_t m = Q(dr + kDrMeshes);
    std::int32_t n = 0, ov = 0, f4 = 0, f8 = 0;
    Rd(m + 0x40, n);
    Rd(dr + kDrOverrides, ov);
    Rd(dr + 0x1A4, f4);
    Rd(dr + 0x1A8, f8);
    const std::uintptr_t arr = Q(m + 0x48);
    Log("[suit]   meshes %p (%s, vt +0x%llX): %d, entries %p; override tables %d; flags +0x1A4 0x%X +0x1A8 0x%X", reinterpret_cast<void*>(m), TypeOf(m), vt(m), n,
        reinterpret_cast<void*>(arr), ov, f4, f8);
    for (int i = 0; i < n && i < 48; ++i) {
        const std::uintptr_t en = arr + static_cast<std::uintptr_t>(i) * kMeshEntry;
        char line[900];
        int k = std::snprintf(line, sizeof(line), "[suit]   mesh %d:", i);
        for (unsigned q = 0; q < kMeshEntry / 8 && k < static_cast<int>(sizeof(line)) - 80; ++q) {
            const std::uintptr_t p = Q(en + 8ull * q);
            if (!p || mem::InImage(p)) continue;
            const char* t = TypeOf(p);
            if (t[0] == '?') {
                // one level deeper: RTTI objects the pointee holds in its first 0x40 bytes
                for (unsigned r = 0; r < 8 && k < static_cast<int>(sizeof(line)) - 80; ++r) {
                    const std::uintptr_t p2 = Q(p + 8ull * r);
                    if (!p2 || mem::InImage(p2)) continue;
                    const char* t2 = TypeOf(p2);
                    if (t2[0] == '?') continue;
                    std::uint8_t gd[16] = {};
                    mem::Read(p2 + 8, gd, 16);
                    char gt[24];
                    GuidText(gd, gt, sizeof(gt));
                    k += std::snprintf(line + k, sizeof(line) - k, " +%X>+%X %s %s;", q * 8, r * 8, t2, gt);
                }
                continue;
            }
            std::uint8_t gd[16] = {};
            mem::Read(p + 8, gd, 16);
            char gt[24];
            GuidText(gd, gt, sizeof(gt));
            k += std::snprintf(line + k, sizeof(line) - k, " +%X %s %s;", q * 8, t, gt);
        }
        Log("%s", line);
    }
}

}  // namespace st::suitcol
