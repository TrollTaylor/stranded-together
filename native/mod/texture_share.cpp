#include "texture_share.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "coop_session.hpp"
#include "link.hpp"
#include "log.hpp"
#include "tex_blob.hpp"
#include "tex_hash.hpp"
#include "tex_slots.hpp"
#include "texture_looks.hpp"
#include "texture_wire.hpp"

namespace st::texshare {
namespace {

namespace tx = st::tex;
constexpr std::uint64_t kCacheMax = 256ull << 20;
// The sender's budget: at most 64 MB of blob bytes per 10 minutes (a full suit set is about 13 MB, one edited 2048 map
// 5.6 MB). Past it the sending pauses (the host keeps the previous look and is told, RESULT kBudget) and resumes with the
// next window: never frozen for the rest of the session.
constexpr std::uint64_t kBudgetBytes = 64ull << 20;
constexpr double kBudgetWindowMs = 600000.0;
// The receiver's own limit (a modified sender is not trusted): 160 MB of accepted pieces per 10 minutes.
constexpr std::uint64_t kRxBudgetBytes = 160ull << 20;

Settings g_cfg;

// ---------------------------------------------------------------- guest (sender)
struct Own {
    bool have = false;
    ftex::Look look;
    int colour = -1;
    int boots = -1;          // the boot type (-1 unknown)
    bool worn = false, own_boots = true;
} g_own;
std::uint8_t g_sent_flags = 0xFF, g_sent_boots = 0xFF, g_sent_worn = 0xFF;
std::uint8_t g_sent_id[32] = {};
bool g_sent = false;
std::uint32_t g_sent_gen = 0xFFFFFFFFu;
std::uint32_t g_epoch = 0;
double g_last_look_try = -1e9;
struct Outgoing {
    std::uint8_t id[32] = {};
    std::uint32_t offset = 0, size = 0;
    double start_ms = 0;
    int retries = 0;
};
std::vector<Outgoing> g_out;
std::vector<std::uint8_t> g_out_bytes;  // the front blob's bytes
std::uint8_t g_out_bytes_id[32] = {};
twire::Bucket g_bucket;
std::uint64_t g_window_bytes = 0;      // sent in the current budget window
double g_window_start_ms = -1;
bool g_budget_told = false;            // the host was told (RESULT kBudget) in this window
std::uint64_t g_chunks_sent = 0, g_blobs_sent = 0;
std::map<std::string, int> g_mismatch_retries;

// ---------------------------------------------------------------- host (receiver)
SRWLOCK g_lock = SRWLOCK_INIT;  // g_partner (read by real_body on the same thread; kept for safety)
ftex::Look g_partner;
int g_partner_boots_style = -1;  // the newest LOOK's flags bit 0 (-1 none yet)
int g_partner_tint = -1;         // the newest LOOK's flags bit 1 (-1 none yet)
twire::Look g_pending;
bool g_have_pending = false;
twire::Look g_last_look;         // the newest LOOK (tex 7 shows it again)
bool g_have_last_look = false;
std::uint32_t g_host_gen = 0;    // the link session the host's state belongs to
std::uint64_t g_rx_window_bytes = 0;
double g_rx_window_start_ms = -1;
bool g_rx_budget_logged = false;
struct Incoming {
    twire::Assembly a;
    int slot = -1;
    std::uint8_t id[32] = {};
    double start_ms = 0, last_ms = 0;
};
std::map<std::string, Incoming> g_in;  // key: blob id hex
std::uint64_t g_chunks_rx = 0, g_blobs_rx = 0, g_refused = 0, g_cache_hits = 0;
bool g_table_logged = false;

std::string Hex8(const std::uint8_t* id) { return tx::Hex(id, 4); }

bool Send(const std::uint8_t* b, std::size_t n) { return n && link::SendAsset(b, n); }

std::wstring CachePath(const std::uint8_t id[32]) {
    const std::string h = tx::Hex(id, 32);
    return g_cfg.cache + L"\\" + std::wstring(h.begin(), h.end()) + L".sttx";
}

bool ReadAll(const std::wstring& path, std::vector<std::uint8_t>& out) {
    const HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    bool ok = GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart <= static_cast<LONGLONG>(twire::kMaxBlob);
    if (ok) {
        out.resize(static_cast<std::size_t>(sz.QuadPart));
        DWORD got = 0;
        ok = ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr) && got == out.size();
    }
    CloseHandle(h);
    if (ok) {  // touch: the LRU keeps what is used
        const HANDLE w = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (w != INVALID_HANDLE_VALUE) {
            FILETIME ft;
            GetSystemTimeAsFileTime(&ft);
            SetFileTime(w, nullptr, nullptr, &ft);
            CloseHandle(w);
        }
    }
    return ok;
}

void CreateDirs(const std::wstring& path) {
    for (std::size_t i = 3; i < path.size(); ++i)
        if (path[i] == L'\\') CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
    CreateDirectoryW(path.c_str(), nullptr);
}

void PruneCache() {
    struct F {
        std::wstring p;
        std::uint64_t size, mtime;
    };
    std::vector<F> files;
    std::uint64_t total = 0;
    WIN32_FIND_DATAW fd{};
    const HANDLE h = FindFirstFileW((g_cfg.cache + L"\\*.sttx").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::uint64_t sz = (static_cast<std::uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        files.push_back(F{g_cfg.cache + L"\\" + fd.cFileName, sz,
                          (static_cast<std::uint64_t>(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime});
        total += sz;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (total <= kCacheMax) return;
    std::sort(files.begin(), files.end(), [](const F& a, const F& b) { return a.mtime < b.mtime; });
    for (const F& f : files) {
        if (total <= kCacheMax) break;
        if (DeleteFileW(f.p.c_str())) total -= f.size;
    }
}

bool SaveToCache(const std::uint8_t id[32], const std::vector<std::uint8_t>& b) {
    if (g_cfg.cache.empty()) return false;
    CreateDirs(g_cfg.cache);
    const std::wstring p = CachePath(id), tmp = p + L".partial";
    const HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    const bool ok = WriteFile(h, b.data(), static_cast<DWORD>(b.size()), &w, nullptr) && w == b.size();
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    PruneCache();
    return true;
}

// The blob from RAM or the disk cache (validated again), or false.
bool Present(const twire::LookEntry& e) {
    if (ftex::HaveBlob(e.id)) return true;
    if (g_cfg.cache.empty()) return false;
    std::vector<std::uint8_t> b;
    if (!ReadAll(CachePath(e.id), b)) return false;
    tx::Sha256 id{};
    std::memcpy(id.data(), e.id, 32);
    std::string why;
    if (!tx::ValidateBlob(tx::kSlots[e.slot], b.data(), b.size(), &id, &why)) {
        Log("[tex] partner cache %s dropped: %s", Hex8(e.id).c_str(), why.c_str());
        DeleteFileW(CachePath(e.id).c_str());
        return false;
    }
    ftex::StoreReceived(e.id, e.slot, std::move(b), CachePath(e.id));
    ++g_cache_hits;
    return true;
}

// The receiver's window (kRxBudgetBytes per kBudgetWindowMs): true while it has room.
bool RxBudgetRoom(double now_ms) {
    if (g_rx_window_start_ms < 0 || now_ms - g_rx_window_start_ms >= kBudgetWindowMs) {
        g_rx_window_start_ms = now_ms;
        g_rx_window_bytes = 0;
        g_rx_budget_logged = false;
    }
    return g_rx_window_bytes <= kRxBudgetBytes;
}

// The host's state of one guest: forgotten on a new link session (another guest may send nothing).
void ResetHost(const char* why) {
    const bool had = !g_partner.Empty() || g_have_pending || g_partner_boots_style >= 0 || !g_in.empty();
    AcquireSRWLockExclusive(&g_lock);
    g_partner = ftex::Look{};
    ReleaseSRWLockExclusive(&g_lock);
    g_partner_boots_style = -1;
    g_partner_tint = -1;
    g_have_pending = false;
    g_have_last_look = false;
    g_in.clear();
    g_rx_window_start_ms = -1;
    g_table_logged = false;
    if (had) Log("[tex] host: the previous guest's look, boots style and half-received textures forgotten (%s)", why);
}

void SendResult(const std::uint8_t id[32], std::uint8_t code) {
    twire::Result r;
    std::memcpy(r.id, id, 32);
    r.code = code;
    std::uint8_t buf[twire::kMaxMessage];
    Send(buf, twire::EncodeResult(r, buf, sizeof(buf)));
    if (code != twire::kOk) {
        ++g_refused;
        Log("[tex] refused %s: %s", Hex8(id).c_str(), twire::CodeName(code));
    }
}

void Promote(const twire::Look& m, unsigned hits) {
    ftex::Look l;
    l.colour = m.colour;
    for (unsigned i = 0; i < m.n; ++i) {
        l.slots.push_back(m.e[i].slot);
        l.blob_ids.insert(l.blob_ids.end(), m.e[i].id, m.e[i].id + 32);
    }
    ftex::looks::LookId(l);
    AcquireSRWLockExclusive(&g_lock);
    g_partner = l;
    ReleaseSRWLockExclusive(&g_lock);
    g_have_pending = false;
    if (l.Empty()) Log("[tex] partner look: her own suit (colour %u) and boots", m.colour);
    else Log("[tex] partner look %s: complete for colour %u (%u blob(s), cache hit %u)", Hex8(l.id).c_str(), m.colour, m.n, hits);
}

bool PendingComplete() {
    for (unsigned i = 0; i < g_pending.n; ++i)
        if (!ftex::HaveBlob(g_pending.e[i].id)) return false;
    return true;
}

void OnLook(const twire::Look& m) {
    const int style = (m.flags & 1u) ? 1 : 0;  // bit 0: her own boots (bootslook::Style::own)
    if (style != g_partner_boots_style)
        Log("[tex] partner boots: %s (boot type %u%s)", style ? "her own boots (no prop), painted with the guest's boot pictures" : "Sam's boots on her feet (the prop)",
            m.boots, m.worn ? ", worn" : "");
    g_partner_boots_style = style;
    const int tint = (m.flags & 2u) ? 1 : 0;  // bit 1: the guest's v0.23 tint (for her own look: both screens the same)
    if (tint != g_partner_tint) Log("[tex] partner tint (fragile_suit_colour on the guest): %s", tint ? "on" : "off");
    g_partner_tint = tint;
    g_last_look = m;
    g_have_last_look = true;
    // only the newest LOOK's blobs are received: half-received ones of an older look are dropped
    for (auto it = g_in.begin(); it != g_in.end();) {
        bool in_look = false;
        for (unsigned i = 0; i < m.n && !in_look; ++i) in_look = std::memcmp(it->second.id, m.e[i].id, 32) == 0;
        if (in_look) ++it;
        else it = g_in.erase(it);
    }
    if (!g_cfg.receive) {
        for (unsigned i = 0; i < m.n; ++i) SendResult(m.e[i].id, twire::kRefused);
        Promote(twire::Look{}, 0);
        return;
    }
    if (m.table != tx::kTableVersion) {
        if (!g_table_logged) Log("[tex] the partner's textures use slot table %08x, this mod %08x: her own suit", m.table, tx::kTableVersion);
        g_table_logged = true;
        for (unsigned i = 0; i < m.n; ++i) SendResult(m.e[i].id, twire::kUnknownSlot);
        Promote(twire::Look{}, 0);
        return;
    }
    for (unsigned i = 0; i < m.n; ++i) {
        const twire::LookEntry& e = m.e[i];
        if (e.slot >= tx::kSlotCount || e.size != tx::BlobSize(tx::kSlots[e.slot])) {
            SendResult(e.id, twire::kUnknownSlot);
            Log("[tex] partner look refused: entry %u (slot %u, %u B) is not one of this build's suit or boots textures", i, e.slot, e.size);
            return;
        }
    }
    Log("[tex] partner look #%u: colour %u, boots %u%s, %u blob(s)", m.epoch, m.colour, m.boots, m.worn ? " worn" : "", m.n);
    twire::IdList have, want;
    have.epoch = want.epoch = m.epoch;
    const std::uint64_t hits0 = g_cache_hits;
    unsigned in_ram = 0, refused = 0;
    const bool room = RxBudgetRoom(static_cast<double>(link::NowMs()));
    for (unsigned i = 0; i < m.n; ++i) {
        const twire::LookEntry& e = m.e[i];
        const bool ram = ftex::HaveBlob(e.id);
        if (ram || Present(e)) {
            in_ram += ram ? 1u : 0u;
            std::memcpy(have.id[have.n++], e.id, 32);
            continue;
        }
        if (!room) {  // the receiver's own limit: the previous look stays
            SendResult(e.id, twire::kBudget);
            ++refused;
            continue;
        }
        const std::string key = tx::Hex(e.id, 32);
        Incoming& in = g_in[key];
        if (in.a.size() != e.size || in.slot != e.slot) {
            in = Incoming{};
            in.a.Init(e.size);
            in.slot = e.slot;
            std::memcpy(in.id, e.id, 32);
            in.start_ms = static_cast<double>(link::NowMs());
        }
        in.last_ms = static_cast<double>(link::NowMs());
        std::memcpy(want.id[want.n], e.id, 32);
        want.offset[want.n++] = in.a.ResumeOffset();
    }
    std::uint8_t buf[twire::kMaxMessage];
    if (have.n) Send(buf, twire::EncodeIds(twire::Kind::have, have, buf, sizeof(buf)));
    if (want.n) Send(buf, twire::EncodeIds(twire::Kind::want, want, buf, sizeof(buf)));
    for (unsigned i = 0; i < want.n; ++i)
        if (want.offset[i]) Log("[tex] partner blob %s: resumed at %u", Hex8(want.id[i]).c_str(), want.offset[i]);
    if (refused) {
        Log("[tex] partner look #%u: %u blob(s) refused, this game's texture budget (%llu MB per 10 min) is used up: the previous look stays", m.epoch,
            refused, static_cast<unsigned long long>(kRxBudgetBytes >> 20));
        return;
    }
    g_pending = m;
    g_have_pending = true;
    if (!want.n) Promote(m, static_cast<unsigned>(g_cache_hits - hits0) + in_ram);
}

void OnChunk(const twire::Chunk& c) {
    Incoming* in = nullptr;
    for (auto& kv : g_in)
        if (kv.second.a.size() && std::memcmp(kv.second.id, c.prefix, 8) == 0) {
            in = &kv.second;
            break;
        }
    if (!in) return;  // not wanted (any more)
    if (!RxBudgetRoom(static_cast<double>(link::NowMs()))) {
        if (!g_rx_budget_logged) Log("[tex] partner textures: this game's texture budget (%llu MB per 10 min) is used up: pieces dropped, the previous look stays",
                                     static_cast<unsigned long long>(kRxBudgetBytes >> 20));
        g_rx_budget_logged = true;
        std::uint8_t idc[32];
        std::memcpy(idc, in->id, 32);
        g_in.erase(tx::Hex(idc, 32));
        SendResult(idc, twire::kBudget);
        return;
    }
    bool added = false;
    try {
        added = in->a.Add(c.offset, c.data, c.len);
    } catch (...) {  // the first piece allocates the blob's buffer
        Log("[tex] partner blob %s: out of memory: dropped", Hex8(in->id).c_str());
        std::uint8_t idc[32];
        std::memcpy(idc, in->id, 32);
        g_in.erase(tx::Hex(idc, 32));
        return;
    }
    if (!added) return;
    g_rx_window_bytes += c.len;
    ++g_chunks_rx;
    in->last_ms = static_cast<double>(link::NowMs());
    if (!in->a.Complete()) return;
    const double ms = static_cast<double>(link::NowMs()) - in->start_ms;
    std::vector<std::uint8_t> b = in->a.Take();
    tx::Sha256 id{};
    std::memcpy(id.data(), in->id, 32);
    std::uint8_t idc[32];
    std::memcpy(idc, in->id, 32);
    const int slot = in->slot;
    g_in.erase(tx::Hex(idc, 32));
    std::string why;
    if (!tx::ValidateBlob(tx::kSlots[slot], b.data(), b.size(), &id, &why)) {
        Log("[tex] partner blob %s: NOT valid (%s), %zu B dropped", Hex8(idc).c_str(), why.c_str(), b.size());
        SendResult(idc, why.find("SHA-256") != std::string::npos ? twire::kHashMismatch : twire::kInvalid);
        return;
    }
    const std::size_t bytes = b.size();
    const bool cached = SaveToCache(idc, b);
    ftex::StoreReceived(idc, slot, std::move(b), cached ? CachePath(idc) : std::wstring());
    ++g_blobs_rx;
    SendResult(idc, twire::kOk);
    Log("[tex] partner look #%u: received %s %s (%zu B in %.0f ms, %.0f KB/s) valid", g_have_pending ? g_pending.epoch : 0u, tx::kSlots[slot].id,
        Hex8(idc).c_str(), bytes, ms, ms > 0 ? bytes / ms * 1000.0 / 1024.0 : 0.0);
    if (g_have_pending && PendingComplete()) Promote(g_pending, 0);
}

void HostTick(double now_ms) {
    std::uint8_t buf[twire::kMaxMessage];
    std::size_t n = 0;
    std::uint32_t gen = 0;
    for (int k = 0; k < 64 && link::PopAsset(buf, sizeof(buf), n, gen); ++k) {
        if (gen > g_host_gen) {  // a new link session: maybe another guest (player_sync's NewSession may come first)
            if (g_host_gen) ResetHost("new link session");
            g_host_gen = gen;
        } else if (gen < g_host_gen) {
            continue;  // from before the session change
        }
        switch (twire::PeekKind(buf, n)) {
        case twire::Kind::look: {
            twire::Look m;
            if (twire::DecodeLook(buf, n, m)) OnLook(m);
            else Log("[tex] a malformed LOOK was dropped (%zu B)", n);
            break;
        }
        case twire::Kind::chunk: {
            twire::Chunk c;
            if (twire::DecodeChunk(buf, n, c)) OnChunk(c);
            break;
        }
        case twire::Kind::result: {  // the guest paused its sending (its budget): the previous look stays meanwhile
            twire::Result r;
            if (twire::DecodeResult(buf, n, r) && r.code == twire::kBudget) {
                Log("[tex] the guest paused sending its textures (%s, %s): the previous look stays until it resumes (within 10 min)", Hex8(r.id).c_str(),
                    twire::CodeName(r.code));
                coop::SetVehicleNotice("Fragile's textures: your partner's newest edits arrive in a few minutes (texture budget)", false, now_ms + 8000.0);
            }
            break;
        }
        default:
            break;
        }
    }
    // incomplete blobs are kept 5 min for a resume, then dropped
    for (auto it = g_in.begin(); it != g_in.end();) {
        if (now_ms - it->second.last_ms > 300000) it = g_in.erase(it);
        else ++it;
    }
    if (ftex::looks::TakeResend()) {
        // tex 7: lets go of the partner look's RAM copies and shows the newest LOOK again: its blobs come from the disk cache
        if (!g_have_last_look) {
            Log("[tex] host: tex 7: no look from the guest yet");
        } else {
            std::vector<const std::uint8_t*> ids;
            for (unsigned i = 0; i < g_last_look.n; ++i) ids.push_back(g_last_look.e[i].id);
            const unsigned dropped = ftex::DropRam(ids);
            Log("[tex] host: tex 7: %u of the guest's %u blob(s) let go of from RAM (read again from the disk cache when needed); look #%u shown again",
                dropped, g_last_look.n, g_last_look.epoch);
            const twire::Look again = g_last_look;
            OnLook(again);
        }
    }
}

// ---------------------------------------------------------------- guest
bool SendLook() {
    twire::Look m;
    m.epoch = ++g_epoch;
    m.table = tx::kTableVersion;
    m.flags = static_cast<std::uint8_t>((g_own.own_boots ? 1u : 0u) | (g_cfg.tint ? 2u : 0u));  // bit 1: our v0.23 tint choice
    m.colour = static_cast<std::uint8_t>(g_own.colour < 0 ? 0 : g_own.colour);
    m.boots = static_cast<std::uint8_t>(g_own.boots < 0 || g_own.boots > 10 ? 0xFF : g_own.boots);
    m.worn = g_own.worn ? 1u : 0u;
    if (g_cfg.send) {
        const std::size_t n = g_own.look.slots.size() < twire::kMaxEntries ? g_own.look.slots.size() : twire::kMaxEntries;
        for (std::size_t i = 0; i < n; ++i) {
            twire::LookEntry& e = m.e[m.n];
            e.slot = static_cast<std::uint8_t>(g_own.look.slots[i]);
            std::memcpy(e.id, g_own.look.blob_ids.data() + 32 * i, 32);
            e.size = static_cast<std::uint32_t>(tx::BlobSize(tx::kSlots[e.slot]));
            ++m.n;
        }
    }
    std::uint8_t buf[twire::kMaxMessage];
    if (!Send(buf, twire::EncodeLook(m, buf, sizeof(buf)))) {
        --g_epoch;
        return false;
    }
    g_out.clear();
    Log("[tex] look #%u sent: colour %u, %u blob(s)%s; boots %u%s, %s", m.epoch, m.colour, m.n, g_cfg.send ? "" : " (textures_send=0: her own suit)", m.boots,
        m.worn ? " worn" : "", m.flags & 1u ? "her own boots" : "Sam's boots (prop)");
    g_sent_flags = m.flags, g_sent_boots = m.boots, g_sent_worn = m.worn;
    return true;
}

void GuestTick(double now_ms, bool in_world) {
    std::uint8_t buf[twire::kMaxMessage];
    std::size_t n = 0;
    std::uint32_t gen = 0;
    for (int k = 0; k < 64 && link::PopAsset(buf, sizeof(buf), n, gen); ++k) {
        switch (twire::PeekKind(buf, n)) {
        case twire::Kind::want: {
            twire::IdList w;
            if (!twire::DecodeIds(buf, n, twire::Kind::want, w) || w.epoch != g_epoch) break;
            for (unsigned i = 0; i < w.n; ++i) {
                bool ours = false;
                for (std::size_t s = 0; s < g_own.look.slots.size(); ++s)
                    if (std::memcmp(g_own.look.blob_ids.data() + 32 * s, w.id[i], 32) == 0) ours = true;
                std::vector<std::uint8_t> b;
                if (!ours || !ftex::BlobBytes(w.id[i], &b)) continue;
                Outgoing o;
                std::memcpy(o.id, w.id[i], 32);
                o.size = static_cast<std::uint32_t>(b.size());
                o.offset = w.offset[i] < o.size ? (w.offset[i] / twire::kChunkData) * static_cast<std::uint32_t>(twire::kChunkData) : 0;
                o.start_ms = now_ms;
                g_out.push_back(o);
                Log("[tex] host wants %s (%u B from %u)", Hex8(o.id).c_str(), o.size, o.offset);
            }
            break;
        }
        case twire::Kind::have: {
            twire::IdList h;
            if (twire::DecodeIds(buf, n, twire::Kind::have, h) && h.n) Log("[tex] host has %u of the look's blob(s) cached", h.n);
            break;
        }
        case twire::Kind::result: {
            twire::Result r;
            if (!twire::DecodeResult(buf, n, r)) break;
            if (r.code == twire::kOk) {
                Log("[tex] host: %s valid", Hex8(r.id).c_str());
                break;
            }
            Log("[tex] host refused %s: %s", Hex8(r.id).c_str(), twire::CodeName(r.code));
            if (r.code == twire::kHashMismatch && g_mismatch_retries[tx::Hex(r.id, 32)]++ < 1) {
                std::vector<std::uint8_t> b;
                if (ftex::BlobBytes(r.id, &b)) {
                    Outgoing o;
                    std::memcpy(o.id, r.id, 32);
                    o.size = static_cast<std::uint32_t>(b.size());
                    o.start_ms = now_ms;
                    g_out.push_back(o);
                }
            }
            break;
        }
        default:
            break;
        }
    }
    // the look: on every change and on each new link session
    const std::uint32_t generation = link::Generation();
    const bool resend = ftex::looks::TakeResend();
    const bool boots_changed = (g_sent_flags & 1u) != (g_own.own_boots ? 1u : 0u) || g_sent_worn != (g_own.worn ? 1u : 0u) ||
                               g_sent_boots != static_cast<std::uint8_t>(g_own.boots < 0 || g_own.boots > 10 ? 0xFF : g_own.boots);
    if (g_own.have && link::GetInfo().connected &&
        (resend || generation != g_sent_gen || !g_sent || boots_changed || std::memcmp(g_sent_id, g_own.look.id, 32) != 0) &&
        now_ms - g_last_look_try > 500) {
        g_last_look_try = now_ms;
        if (generation != g_sent_gen) g_window_start_ms = -1;  // a new session starts a new budget window
        if (SendLook()) {
            g_sent = true;
            g_sent_gen = generation;
            std::memcpy(g_sent_id, g_own.look.id, 32);
        }
    }
    // the wanted blobs, paced; nothing while the save channel is busy (the host-held save first)
    if (g_out.empty() || link::SaveBusy()) return;
    if (g_window_start_ms < 0 || now_ms - g_window_start_ms >= kBudgetWindowMs) {
        if (g_window_start_ms >= 0 && g_window_bytes > kBudgetBytes) Log("[tex] a new texture budget window: sending resumes");
        g_window_start_ms = now_ms;
        g_window_bytes = 0;
        g_budget_told = false;
    }
    if (g_window_bytes > kBudgetBytes) {  // paused (not dropped) until the next window; the host is told once
        if (!g_budget_told) {
            g_budget_told = true;
            Log("[tex] the texture budget (%llu MB per 10 min) is used up: sending paused for %.0f s, the host keeps the previous look",
                static_cast<unsigned long long>(kBudgetBytes >> 20), (kBudgetWindowMs - (now_ms - g_window_start_ms)) / 1000.0);
            for (const Outgoing& o : g_out) {
                twire::Result r;
                std::memcpy(r.id, o.id, 32);
                r.code = twire::kBudget;
                Send(buf, twire::EncodeResult(r, buf, sizeof(buf)));
            }
            coop::SetVehicleNotice("Fragile's textures: your newest edits reach your partner in a few minutes (texture budget)", false, now_ms + 8000.0);
        }
        return;
    }
    const unsigned limit = in_world ? 8u : 24u;
    const double rate = in_world ? 128.0 * 1024.0 : 400.0 * 1024.0;
    unsigned queued = link::AssetsQueued();
    while (!g_out.empty() && queued < limit) {
        Outgoing& o = g_out.front();
        if (std::memcmp(g_out_bytes_id, o.id, 32) != 0 || g_out_bytes.empty()) {
            if (!ftex::BlobBytes(o.id, &g_out_bytes)) {
                g_out.erase(g_out.begin());
                continue;
            }
            std::memcpy(g_out_bytes_id, o.id, 32);
        }
        if (g_window_bytes > kBudgetBytes) return;  // paused (above, at the next tick)
        const std::uint32_t len = static_cast<std::uint32_t>(std::min<std::size_t>(twire::kChunkData, o.size - o.offset));
        if (!g_bucket.Take(now_ms, rate, 4.0 * twire::kChunkData, len)) return;
        twire::Chunk c;
        std::memcpy(c.prefix, o.id, 8);
        c.offset = o.offset;
        c.len = static_cast<std::uint16_t>(len);
        c.data = g_out_bytes.data() + o.offset;
        if (!Send(buf, twire::EncodeChunk(c, buf, sizeof(buf)))) return;
        ++queued;
        ++g_chunks_sent;
        g_window_bytes += len;
        o.offset += len;
        if (o.offset >= o.size) {
            ++g_blobs_sent;
            Log("[tex] sent %s: %u B in %.0f ms", Hex8(o.id).c_str(), o.size, now_ms - o.start_ms);
            g_out.erase(g_out.begin());
        }
    }
}

}  // namespace

void Configure(const Settings& s) noexcept {
    g_cfg = s;
    if (s.on) Log("[tex] texture sharing: %s (%s)", s.guest ? "guest: sends its look" : "host: shows the guest's look", s.guest ? (s.send ? "send on" : "textures_send=0")
                                                                                                                                : (s.receive ? "receive on" : "textures_receive=0"));
}

void Tick(double now_ms, bool in_world) noexcept {
    if (!g_cfg.on) return;
    if (g_cfg.guest) GuestTick(now_ms, in_world);
    else HostTick(now_ms);
}

void SetOwnLook(bool have, const ftex::Look& look, int colour) noexcept {
    if (!have) return;
    g_own.have = true;
    g_own.look = look;
    g_own.colour = colour;
}

void SetOwnBoots(int type, bool worn, bool own_style) noexcept {
    g_own.boots = type;
    g_own.worn = worn;
    g_own.own_boots = own_style;
}

int PartnerBootsStyle() noexcept { return g_partner_boots_style; }
int PartnerTint() noexcept { return g_partner_tint; }

void NewSession(std::uint32_t generation) noexcept {
    if (!g_cfg.on || g_cfg.guest || generation <= g_host_gen) return;
    if (g_host_gen) ResetHost("new link session");
    g_host_gen = generation;
}

bool PartnerLook(ftex::Look* out) noexcept {
    AcquireSRWLockShared(&g_lock);
    *out = g_partner;
    ReleaseSRWLockShared(&g_lock);
    return true;
}

void LogStatus() noexcept {
    std::uint64_t sent = 0, rx = 0;
    link::AssetCounts(&sent, &rx);
    Log("[tex] link: %s; messages sent %llu received %llu; chunks sent %llu (blobs %llu), chunks received %llu (blobs %llu), refused %llu, cache hits %llu, "
        "incoming %zu, outgoing %zu",
        !g_cfg.on ? "off" : g_cfg.guest ? "guest" : "host", static_cast<unsigned long long>(sent), static_cast<unsigned long long>(rx),
        static_cast<unsigned long long>(g_chunks_sent), static_cast<unsigned long long>(g_blobs_sent), static_cast<unsigned long long>(g_chunks_rx),
        static_cast<unsigned long long>(g_blobs_rx), static_cast<unsigned long long>(g_refused), static_cast<unsigned long long>(g_cache_hits), g_in.size(),
        g_out.size());
}

}  // namespace st::texshare
