#include "texture_looks.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "boots_look.hpp"
#include "extract_plan.hpp"
#include "log.hpp"
#include "logo_recipe.hpp"
#include "logo_slots.hpp"
#include "png_wic.hpp"
#include "ready_made_rules.hpp"
#include "suit_defaults.hpp"
#include "suit_colour_math.hpp"
#include "tex_blob.hpp"
#include "tex_hash.hpp"
#include "tex_manifest.hpp"
#include "tex_slots.hpp"

namespace st::ftex::looks {
namespace {

namespace tx = st::tex;
namespace df = st::tex::defaults;
namespace rm = st::tex::readymade;

constexpr char kSuitDir[] = "Fragile porter suit";
constexpr char kBootsDir[] = "Fragile boots";
constexpr char kOriginal[] = "Original";
constexpr std::uint64_t kBuiltCacheMax = 256ull << 20;

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

bool ReadAll(const std::wstring& path, std::vector<std::uint8_t>& out) {
    const HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    bool ok = GetFileSizeEx(h, &sz) && sz.QuadPart >= 0 && sz.QuadPart < (256ll << 20);
    if (ok) {
        out.resize(static_cast<std::size_t>(sz.QuadPart));
        DWORD got = 0;
        ok = out.empty() || (ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr) && got == out.size());
    }
    CloseHandle(h);
    return ok;
}

bool WriteAtomic(const std::wstring& path, const std::vector<std::uint8_t>& data) {
    const std::wstring tmp = path + L".partial";
    const HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    const bool ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &w, nullptr) && w == data.size();
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

struct FileStamp {
    std::uint64_t size = 0, mtime = 0;
};
bool Stamp(const std::wstring& path, FileStamp& s) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    s.size = (static_cast<std::uint64_t>(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
    s.mtime = (static_cast<std::uint64_t>(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime;
    return true;
}

// ---------------------------------------------------------------- state (g_lock)
SRWLOCK g_lock = SRWLOCK_INIT;
Settings g_cfg;
bool g_started = false;
std::atomic<int> g_wanted{-1};
std::atomic<int> g_pattern{0};
std::atomic<bool> g_rescan{false}, g_resend{false};
std::atomic<unsigned> g_built{0};
struct ColourLook {
    bool settled = false;  // built at least once (OwnLook answers with `look`)
    bool stale = false;    // the folder changed since: rebuilt when wanted, the old look answers meanwhile (no flicker)
    Look look;
    std::string sig;  // the inputs' hashes the look was built from
};
ColourLook g_looks[17];
ColourLook g_boots_looks[bootslook::kKeyCount];  // by bootslook::Key
std::atomic<int> g_boots_wanted{-1};
Look g_pattern_look;
int g_pattern_built = 0;
const char* g_state = "idle";  // a literal: set under the lock without allocating
HANDLE g_wake = nullptr;       // auto-reset: a new wanted colour or a host request wakes the worker at once

// ---- round 8: the ready-made colours (suit_defaults.hpp)
std::atomic<int> g_variant{0};     // tex 11: the metal variant (0 = the product)
std::atomic<int> g_make_mode{0};   // tex 12 (host): 0 product, 1 never make here, 2 pretend the originals differ
std::atomic<unsigned> g_made_here{0}, g_made_failed{0}, g_ready_built{0};
// the guest's ready-made blobs (blob id hex -> what they are): announced as a RECIPE (texture_share)
std::map<std::string, Recipe> g_recipes;   // g_lock
// host: requests to make a guest's ready-made blob here, and the results (g_lock)
struct MakeRequest {
    Recipe r;
    std::uint8_t id[32] = {};
    std::wstring partner_cache;
    std::uint64_t cache_max = 0;   // the partner cache's cap (texture_share's), pruned after a made blob is written
};
std::vector<MakeRequest> g_make_queue;
// Every request taken from the queue gives exactly one result (made, failed, or failed by an exception): texture_share
// waits for the ids of its pending look, so a result is never dropped (the game thread takes them every tick; the
// queue holds only the newest look's requests, DropQueuedMakes).
std::vector<Made> g_made;

// worker-only
struct HashEntry {
    FileStamp st;
    std::string sha;
};
std::map<std::wstring, HashEntry> g_hashes;
tx::Manifest g_manifest;
bool g_manifest_ok = false;
FileStamp g_manifest_stamp;
// the ready-made colours' inputs (worker-only): her Original pictures' key (SHA-256 of their files' hashes), the analysis
// of her Colour for that key, the originals' identity H (suit_defaults.hpp OriginalsId) per key
std::string g_an_key;
df::Analysis g_an;
std::map<std::string, tx::Sha256> g_orig_ids;
bool g_rm_off_logged = false;

void SetState(const char* s) {
    AcquireSRWLockExclusive(&g_lock);
    g_state = s;
    ReleaseSRWLockExclusive(&g_lock);
}

std::wstring Dir() { return g_cfg.dir; }

// SHA-256 hex of a file, re-hashed only when its size or time changed. "" when missing.
std::string FileSha(const std::wstring& path) {
    FileStamp st;
    if (!Stamp(path, st)) return {};
    auto it = g_hashes.find(path);
    if (it != g_hashes.end() && it->second.st.size == st.size && it->second.st.mtime == st.mtime) return it->second.sha;
    tx::Sha256 h{};
    if (!tx::HashFileSha256(path, h)) return {};
    HashEntry e;
    e.st = st;
    e.sha = tx::Hex(h);
    g_hashes[path] = e;
    return e.sha;
}

bool LoadManifest() {
    const std::wstring p = Dir() + L"\\" + Wide(tx::kManifestName);
    FileStamp st;
    if (!Stamp(p, st)) {
        if (g_manifest_ok || g_manifest_stamp.size != 1) Log("[tex] no %s in the textures folder (not extracted yet: the launcher extracts by itself when it starts, or press Extract textures): her own look until it is there", tx::kManifestName);
        g_manifest_ok = false;
        g_manifest_stamp = FileStamp{1, 0};
        return false;
    }
    if (g_manifest_ok && st.size == g_manifest_stamp.size && st.mtime == g_manifest_stamp.mtime) return true;
    g_manifest_stamp = st;
    std::vector<std::uint8_t> b;
    tx::Manifest m;
    std::string err;
    if (!ReadAll(p, b) || !tx::ParseManifest(std::string(b.begin(), b.end()), m, err)) {
        Log("[tex] off: the textures folder's manifest does not read (%s): press Extract textures again", err.c_str());
        g_manifest_ok = false;
        return false;
    }
    if (m.table != tx::kTableVersion) {
        Log("[tex] off: textures were extracted by another version (table %08x, this mod %08x): press Extract textures", m.table, tx::kTableVersion);
        g_manifest_ok = false;
        return false;
    }
    g_manifest = std::move(m);
    g_manifest_ok = true;
    Log("[tex] ready: manifest table %08x ok, %zu file(s)", g_manifest.table, g_manifest.rows.size());
    return true;
}

// The launcher's extraction holds tx::kExtractLockName while it writes (extract_plan.hpp): the folder is then half the
// new pictures, half the old ones, against the old index (a refreshed unpainted picture would look painted, and the
// extractor's replacements would meet the mod's reads). Looks and the host's makes wait for its end (the current look
// stays meanwhile), at most 2 minutes. The worker thread only.
ULONGLONG g_extract_seen = 0;
bool ExtractionRunning() {
    if (g_cfg.dir.empty()) return false;
    const HANDLE h = CreateFileW((Dir() + L"\\" + Wide(tx::kExtractLockName)).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, 0, nullptr);
    bool running = false;
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);   // a leftover nobody holds: not running
    else running = GetLastError() == ERROR_SHARING_VIOLATION;
    const ULONGLONG now = GetTickCount64();
    if (!running) {
        if (g_extract_seen) Log("[tex] the launcher's extraction ended after %llu ms: her looks are read from the folder again", now - g_extract_seen);
        g_extract_seen = 0;
        return false;
    }
    if (!g_extract_seen) {
        g_extract_seen = now;
        Log("[tex] the launcher is extracting the textures: her looks wait for it to end (the current one stays meanwhile)");
    }
    return now - g_extract_seen < 120000;
}

// The colour's editable folder (matched by its "NN " prefix), relative ("Fragile porter suit\16 Gold"), or "".
std::string ColourFolder(int colour) {
    const std::wstring base = Dir() + L"\\" + Wide(kSuitDir);
    WIN32_FIND_DATAW fd{};
    const HANDLE h = FindFirstFileW((base + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::string found;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        const std::string name = Utf8(fd.cFileName);
        if (tx::FolderNumber(name) == colour) {
            found = std::string(kSuitDir) + "\\" + name;
            break;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

std::string FileName(const std::string& rel) {
    const std::size_t p = rel.find_last_of("\\/");
    return p == std::string::npos ? rel : rel.substr(p + 1);
}
std::string ParentName(const std::string& rel) {
    const std::size_t p = rel.find_last_of("\\/");
    if (p == std::string::npos) return {};
    const std::size_t q = rel.find_last_of("\\/", p - 1);
    return rel.substr(q == std::string::npos ? 0 : q + 1, p - (q == std::string::npos ? 0 : q + 1));
}

std::vector<int> ChannelsOf(const std::string& component) {
    std::vector<int> ch;
    for (char c : component) {
        if (c == 'R') ch.push_back(0);
        else if (c == 'G') ch.push_back(1);
        else if (c == 'B') ch.push_back(2);
        else if (c == 'A') ch.push_back(3);
    }
    return ch;
}

bool StartsWithI(const std::string& s, const std::string& p) {
    if (s.size() < p.size()) return false;
    for (std::size_t i = 0; i < p.size(); ++i) {
        char a = s[i], b = p[i];
        if (a == '/') a = '\\';
        if (b == '/') b = '\\';
        if (std::tolower(static_cast<unsigned char>(a)) != std::tolower(static_cast<unsigned char>(b))) return false;
    }
    return true;
}

// The built textures' cache folder (Settings.cache: the mod's own folder, never the textures folder); "" = none.
std::wstring CacheDir() { return g_cfg.cache; }

void CreateDirs(const std::wstring& path) {
    for (std::size_t i = 3; i < path.size(); ++i)
        if (path[i] == L'\\') CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
    CreateDirectoryW(path.c_str(), nullptr);
}

// The oldest (by write time) *.sttx of `dir` deleted until the folder holds at most `max` bytes.
void PruneDir(const std::wstring& dir, std::uint64_t max) {
    if (dir.empty()) return;
    struct F {
        std::wstring p;
        std::uint64_t size, mtime;
    };
    std::vector<F> files;
    std::uint64_t total = 0;
    WIN32_FIND_DATAW fd{};
    const HANDLE h = FindFirstFileW((dir + L"\\*.sttx").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::uint64_t sz = (static_cast<std::uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        files.push_back(F{dir + L"\\" + fd.cFileName, sz,
                          (static_cast<std::uint64_t>(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime});
        total += sz;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (total <= max) return;
    std::sort(files.begin(), files.end(), [](const F& a, const F& b) { return a.mtime < b.mtime; });
    for (const F& f : files) {
        if (total <= max) break;
        if (DeleteFileW(f.p.c_str())) total -= f.size;
    }
}
void PruneBuiltCache() { PruneDir(CacheDir(), kBuiltCacheMax); }

// Builds (or takes from the cache) the blob of one slot from its component pictures. Stores it; fills the blob id.
bool BuildSlot(int slot, const std::vector<std::pair<std::wstring, std::string>>& comps, const std::vector<std::string>& components,
               const std::string& key_hex, std::uint8_t id_out[32]) {
    const tx::Slot& s = tx::kSlots[slot];
    const std::wstring cache_dir = CacheDir();
    const std::wstring cache_file = cache_dir.empty() ? std::wstring() : cache_dir + L"\\" + Wide(key_hex) + L".sttx";
    std::vector<std::uint8_t> blob;
    std::string why;
    if (!cache_file.empty() && ReadAll(cache_file, blob) && tx::ValidateBlob(s, blob.data(), blob.size(), nullptr, &why)) {
        tx::Sha256 h{};
        tx::HashSha256(blob.data(), blob.size(), h);
        std::memcpy(id_out, h.data(), 32);
        StoreBlob(id_out, slot, std::move(blob), cache_file);
        Log("[tex] %s %s from the cache", s.id, tx::Hex(id_out, 4).c_str());
        return true;
    }
    const ULONGLONG t0 = GetTickCount64();
    tx::Image packed;
    packed.Resize(s.w, s.h);
    // channels nobody fills stay 0, alpha 255 (BC1 / BC5 ignore it)
    for (std::size_t i = 3; i < packed.rgba.size(); i += 4) packed.rgba[i] = 255;
    for (std::size_t i = 0; i < comps.size(); ++i) {
        tx::Image img;
        std::string err;
        // any picture WIC reads, by its content (an image tool may save a JPEG under the .png name). The size from the
        // header first: a picture far bigger than the slot is refused before it is decoded (a 16384 x 16384 picture would
        // need 1 GiB in the game's process)
        std::vector<std::uint8_t> pic;
        std::uint32_t pw = 0, ph = 0;
        tx::PictureFormat fmt = tx::PictureFormat::unknown;
        if (!ReadAll(comps[i].first, pic)) {
            Log("[tex] %s: %s cannot be read: slot skipped", s.id, comps[i].second.c_str());
            return false;
        }
        if (!tx::PictureSize(pic.data(), pic.size(), pw, ph, &fmt, &err)) {
            Log("[tex] %s: %s does not read as a picture (%s): slot skipped", s.id, comps[i].second.c_str(), err.c_str());
            return false;
        }
        if (fmt != tx::PictureFormat::png)
            Log("[tex] %s is a %s saved with a .png name: read anyway (%ux%u%s)", comps[i].second.c_str(), tx::PictureFormatName(fmt), pw, ph,
                fmt == tx::PictureFormat::jpeg ? ", lossy" : "");
        if (pw > tx::MaxEditSide(s.w) || ph > tx::MaxEditSide(s.h)) {
            Log("[tex] %s: %s is %ux%u, too big (at most %ux%u; save it at %ux%u): slot skipped", s.id, comps[i].second.c_str(), pw, ph, tx::MaxEditSide(s.w),
                tx::MaxEditSide(s.h), s.w, s.h);
            return false;
        }
        if (!tx::DecodePicture(pic.data(), pic.size(), img, tx::MaxEditSide(s.w), tx::MaxEditSide(s.h), nullptr, &err)) {
            Log("[tex] %s: %s does not read as a %s (%s): slot skipped", s.id, comps[i].second.c_str(), tx::PictureFormatName(fmt), err.c_str());
            return false;
        }
        std::vector<std::uint8_t>().swap(pic);
        if (img.w != s.w || img.h != s.h) {
            Log("[tex] %s is %ux%u, resized to %ux%u", comps[i].second.c_str(), img.w, img.h, s.w, s.h);
            img = tx::Resize(img, s.w, s.h, components[i] == "RGB" && s.srgb ? tx::MipKind::srgb_rgb : tx::MipKind::linear);
        }
        if (!tx::InsertChannels(packed, img, ChannelsOf(components[i]))) {
            Log("[tex] %s: %s could not be packed", s.id, comps[i].second.c_str());
            return false;
        }
    }
    if (!tx::BuildBlob(s, packed, blob, &why)) {
        Log("[tex] %s: the blob was not built (%s)", s.id, why.c_str());
        return false;
    }
    tx::Sha256 h{};
    tx::HashSha256(blob.data(), blob.size(), h);
    std::memcpy(id_out, h.data(), 32);
    bool cached = false;
    if (!cache_file.empty()) {
        CreateDirs(cache_dir);
        cached = WriteAtomic(cache_file, blob);
        if (!cached) Log("[tex] %s: the built texture was not cached (the folder is not writable?)", s.id);
        else PruneBuiltCache();
    }
    const std::size_t bytes = blob.size();
    StoreBlob(id_out, slot, std::move(blob), cached ? cache_file : std::wstring());
    g_built.fetch_add(1);
    Log("[tex] built %s %s in %llu ms (%zu B)", s.id, tx::Hex(id_out, 4).c_str(), static_cast<unsigned long long>(GetTickCount64() - t0), bytes);
    return true;
}

// ---------------------------------------------------------------- the ready-made colours (round 8)
// Her Original pictures the generator and the slots read, by file name.
constexpr const char* kOrigNames[4] = {"Suit - Colour.png", "Suit - Reflectance.png", "Suit - Roughness.png", "Suit - Ambient occlusion.png"};

// Decoded Original pictures, kept only for one build (about 40 MB for the four).
struct OrigImages {
    std::map<std::string, tx::Image> img;
};
// One colour's ready-made work (a colour's S0 and S1, its originals' identity, or a host's consecutive requests): her
// Original pictures decoded once and the generator's output made once, then freed with the context.
struct ReadyCtx {
    OrigImages oi;
    std::string key;           // the originals' key the output below was made from
    int colour = -1, variant = -1;
    df::Output out;
    bool made = false;
};
std::string OrigRel(const std::string& name) { return std::string(kSuitDir) + "\\" + kOriginal + "\\" + name; }

// The key of her Original pictures: each must still be what the extraction wrote (the manifest's hash). "" + why.
std::string OriginalsKey(std::string* why) {
    std::string text = "stx-orig";
    for (const char* n : kOrigNames) {
        const tx::ManifestRow* r = g_manifest.Find(OrigRel(n));
        if (!r) return *why = std::string("no ") + n + " in Original\\ (extract the textures again)", std::string();
        const std::string sha = FileSha(Dir() + L"\\" + Wide(r->path));
        if (sha.empty()) return *why = std::string(n) + " is missing from Original\\ (extract the textures again)", std::string();
        if (sha != r->sha256) return *why = std::string(n) + " in Original\\ changed since the extraction (extract the textures again)", std::string();
        text += "|" + sha;
    }
    tx::Sha256 h{};
    tx::HashSha256(text.data(), text.size(), h);
    return tx::Hex(h);
}

const tx::Image* OrigImage(OrigImages& oi, const std::string& name, std::string* why) {
    auto it = oi.img.find(name);
    if (it != oi.img.end()) return &it->second;
    tx::Image img;
    std::string err;
    if (!tx::ReadPngFile(Dir() + L"\\" + Wide(OrigRel(name)), img, &err)) return *why = name + " in Original\\ does not read (" + err + ")", nullptr;
    return &(oi.img[name] = std::move(img));
}

bool EnsureAnalysis(const std::string& key, OrigImages& oi, unsigned* ms, std::string* why) {
    if (g_an_key == key && g_an.w) return true;
    const ULONGLONG t0 = GetTickCount64();
    const tx::Image* c = OrigImage(oi, kOrigNames[0], why);
    if (!c) return false;
    g_an_key.clear();
    if (!df::Analyse(*c, g_an, why)) return false;
    g_an_key = key;
    if (ms) *ms = static_cast<unsigned>(GetTickCount64() - t0);
    return true;
}

// H of her Original pictures (the RECIPE's precheck): from memory, <cache>\originals.txt, or decoded now.
bool EnsureOriginalsId(const std::string& key, OrigImages& oi, tx::Sha256& out, std::string* why) {
    auto it = g_orig_ids.find(key);
    if (it != g_orig_ids.end()) return out = it->second, true;
    const std::wstring file = CacheDir().empty() ? std::wstring() : CacheDir() + L"\\originals.txt";
    std::vector<std::uint8_t> b;
    if (!file.empty() && ReadAll(file, b)) {
        const std::string text(b.begin(), b.end());
        for (std::size_t p = 0; p < text.size();) {
            const std::size_t e = text.find('\n', p);
            const std::string line = text.substr(p, e == std::string::npos ? std::string::npos : e - p);
            p = e == std::string::npos ? text.size() : e + 1;
            if (line.size() >= 129 && line.compare(0, 64, key) == 0 && line[64] == ' ') {
                tx::Sha256 h{};
                bool ok = true;
                for (int i = 0; i < 32 && ok; ++i) {
                    unsigned v = 0;
                    ok = std::sscanf(line.c_str() + 65 + 2 * i, "%2x", &v) == 1;
                    h[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v);
                }
                if (ok && tx::Hex(h) == line.substr(65, 64)) return out = g_orig_ids[key] = h, true;
            }
        }
    }
    const tx::Image* imgs[4] = {};
    for (int i = 0; i < 4; ++i)
        if (!(imgs[i] = OrigImage(oi, kOrigNames[i], why))) return false;
    out = df::OriginalsId(*imgs[0], *imgs[1], *imgs[2], *imgs[3]);
    g_orig_ids[key] = out;
    if (!file.empty()) {
        CreateDirs(CacheDir());
        std::string text(b.begin(), b.end());
        if (text.size() > 64 * 1024) text.clear();   // a few lines per game update: never grows for long
        text += key + " " + tx::Hex(out) + "\n";
        WriteAtomic(file, std::vector<std::uint8_t>(text.begin(), text.end()));
    }
    return true;
}

// The ready-made blob of one suit slot (S0 / S1) for a colour, made from her Original pixels (no file of the colour
// folder is read). No cache: the caller decides (the guest caches, the host never reads a cache for a recipe).
bool MakeReadyBlob(int si, int colour, int variant, const std::string& key, ReadyCtx& ctx, std::vector<std::uint8_t>& blob, unsigned* an_ms,
                   std::string* why) {
    const tx::Slot& s = tx::kSlots[si];
    OrigImages& oi = ctx.oi;
    if (!EnsureAnalysis(key, oi, an_ms, why)) return false;
    if (!(ctx.made && ctx.key == key && ctx.colour == colour && ctx.variant == variant)) {
        ctx.made = false;
        const tx::Image* c = OrigImage(oi, kOrigNames[0], why);
        if (!c) return false;
        static const tx::Image kNone;
        const tx::Image* f = &kNone;
        const tx::Image* r = &kNone;
        if (colour == 15 || colour == 16) {
            if (!(f = OrigImage(oi, kOrigNames[1], why)) || !(r = OrigImage(oi, kOrigNames[2], why))) return false;
        }
        if (!df::Make(colour, variant, g_an, *c, *f, *r, ctx.out, why)) return false;
        ctx.key = key, ctx.colour = colour, ctx.variant = variant, ctx.made = true;
    }
    const df::Output& out = ctx.out;
    tx::Image packed;
    packed.Resize(s.w, s.h);
    for (std::size_t i = 3; i < packed.rgba.size(); i += 4) packed.rgba[i] = 255;
    bool any = false;
    for (const tx::ManifestRow& row : g_manifest.rows) {
        if (row.kind != "original" || row.slot != s.id || !StartsWithI(row.path, std::string(kSuitDir) + "\\" + kOriginal + "\\")) continue;
        const std::string name = FileName(row.path);
        const tx::Image* img = nullptr;
        if ((out.maps & df::kColour) && tx::SamePath(name, kOrigNames[0])) img = &out.colour;
        else if ((out.maps & df::kReflectance) && tx::SamePath(name, kOrigNames[1])) img = &out.reflectance;
        else if ((out.maps & df::kRoughness) && tx::SamePath(name, kOrigNames[2])) img = &out.roughness;
        else img = OrigImage(oi, name, why);
        if (!img) return false;
        tx::Image resized;
        if (img->w != s.w || img->h != s.h) {
            resized = tx::Resize(*img, s.w, s.h, row.component == "RGB" && s.srgb ? tx::MipKind::srgb_rgb : tx::MipKind::linear);
            img = &resized;
        }
        if (!tx::InsertChannels(packed, *img, ChannelsOf(row.component))) return *why = s.id + std::string(": ") + name + " could not be packed", false;
        any = true;
    }
    if (!any) return *why = std::string("no Original pictures of ") + s.id + " in the manifest", false;
    std::string err;
    if (!tx::BuildBlob(s, packed, blob, &err)) return *why = std::string(s.id) + ": the blob was not built (" + err + ")", false;
    return true;
}

void ReadyMadeOff(const std::string& why) {
    if (g_rm_off_logged) return;
    g_rm_off_logged = true;
    Log("[tex] ready-made colours off: %s (Fragile wears her own suit in the colours you did not paint)", why.c_str());
}

// Guest: the ready-made slot `si` of `colour`, from the built cache or made now; registered as a recipe and, with
// `store`, stored for the look (a yours folder's comparison only needs the id).
bool BuildDefaultSlot(int si, int colour, std::uint8_t id_out[32], ReadyCtx& ctx, bool store = true) {
    const tx::Slot& s = tx::kSlots[si];
    const int variant = colour >= 15 ? g_variant.load() : 0;
    std::string why;
    const std::string key = OriginalsKey(&why);
    if (key.empty()) return ReadyMadeOff(why), false;
    char kt[256];
    std::snprintf(kt, sizeof(kt), "stx-default|%d|%u|%s|%d|%d|%s", df::kGeneratorVersion, tx::kBlobEncoderVersion, s.id, colour, variant, key.c_str());
    tx::Sha256 kh{};
    tx::HashSha256(kt, std::strlen(kt), kh);
    const std::wstring cache_dir = CacheDir();
    const std::wstring cache_file = cache_dir.empty() ? std::wstring() : cache_dir + L"\\" + Wide(tx::Hex(kh)) + L".sttx";
    std::vector<std::uint8_t> blob;
    const ULONGLONG t0 = GetTickCount64();
    unsigned an_ms = 0, enc_ms = 0;
    bool from_cache = false;
    if (!cache_file.empty() && ReadAll(cache_file, blob) && tx::ValidateBlob(s, blob.data(), blob.size(), nullptr, &why)) {
        from_cache = true;
    } else {
        blob.clear();
        const ULONGLONG t1 = GetTickCount64();
        if (!MakeReadyBlob(si, colour, variant, key, ctx, blob, &an_ms, &why)) {
            Log("[tex] ready-made %s for colour %d not made: %s", s.id, colour, why.c_str());
            ReadyMadeOff(why);
            return false;
        }
        enc_ms = static_cast<unsigned>(GetTickCount64() - t1);
    }
    // the originals' identity for the RECIPE (the host makes the same blob from its own copy of these pictures)
    tx::Sha256 h{};
    if (!EnsureOriginalsId(key, ctx.oi, h, &why)) {
        Log("[tex] ready-made %s: the originals' identity was not computed (%s): sent as a blob", s.id, why.c_str());
        std::memset(h.data(), 0, 32);
    }
    tx::Sha256 idh{};
    tx::HashSha256(blob.data(), blob.size(), idh);
    std::memcpy(id_out, idh.data(), 32);
    bool cached = from_cache;
    if (!from_cache && !cache_file.empty()) {
        CreateDirs(cache_dir);
        cached = WriteAtomic(cache_file, blob);
        if (cached) PruneBuiltCache();
    }
    if (store) StoreBlob(id_out, si, std::move(blob), cached ? cache_file : std::wstring());
    Recipe r;
    r.slot = si;
    r.colour = colour;
    r.variant = variant;
    std::memcpy(r.originals, h.data(), 32);
    r.valid = h != tx::Sha256{};
    const std::string idhex = tx::Hex(id_out, 32);
    AcquireSRWLockExclusive(&g_lock);
    g_recipes[idhex] = r;
    ReleaseSRWLockExclusive(&g_lock);
    if (from_cache) {
        Log("[tex] ready-made %s %s for colour %d from the cache", s.id, tx::Hex(id_out, 4).c_str(), colour);
    } else {
        g_ready_built.fetch_add(1);
        g_built.fetch_add(1);
        Log("[tex] ready-made %s %s for colour %d made in %llu ms (analysis %u ms, make + encode %u ms)%s", s.id, tx::Hex(id_out, 4).c_str(), colour,
            static_cast<unsigned long long>(GetTickCount64() - t0), an_ms, enc_ms, variant ? " (metal variant)" : "");
    }
    return true;
}

void PushMade(const Made& m) {
    bool lost = false;
    AcquireSRWLockExclusive(&g_lock);
    try {
        g_made.push_back(m);   // reserved when the worker starts: no allocation in practice
    } catch (...) {
        lost = true;
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (lost) Log("[tex] host: a make result was lost (out of memory): the partner's look waits until the guest sends it again");
}

// Host: one guest blob made here from this game's own Original pictures (never a cache), checked against its id.
void MakeForPartnerNow(const MakeRequest& q, ReadyCtx& ctx) {
    Made m;
    std::memcpy(m.id, q.id, 32);
    const ULONGLONG t0 = GetTickCount64();
    std::string why;
    std::vector<std::uint8_t> blob;
    std::string key;
    tx::Sha256 h{};
    // hidden test hooks (tex 12): 3 every make waits 2 s first (a slow PC: the newest look must not wait for older ones),
    // 4 the next make throws as an out-of-memory would (its request still gets a result)
    if (g_make_mode.load() == 3) Sleep(2000);
    if (g_make_mode.load() == 4) {
        g_make_mode.store(0);
        throw std::bad_alloc();
    }
    if (g_cfg.dir.empty()) why = "no textures folder here";
    else if (!LoadManifest()) why = "the textures are not extracted here yet";
    else if ((key = OriginalsKey(&why)).empty()) {
    } else if (!EnsureOriginalsId(key, ctx.oi, h, &why)) {
    } else if (g_make_mode.load() == 2 || std::memcmp(h.data(), q.r.originals, 32) != 0) {
        why = g_make_mode.load() == 2 ? "originals differ (tex 12 2)" : "originals differ (another game version?)";
    } else if (q.r.slot < 0 || q.r.slot >= static_cast<int>(tx::kSlotCount) ||
               !MakeReadyBlob(q.r.slot, q.r.colour, q.r.variant, key, ctx, blob, nullptr, &why)) {
        if (why.empty()) why = "unknown slot";
    } else {
        tx::Sha256 idh{};
        tx::HashSha256(blob.data(), blob.size(), idh);
        if (std::memcmp(idh.data(), q.id, 32) != 0) {
            why = "made here, differs";
        } else {
            std::wstring path;
            if (!q.partner_cache.empty()) {
                CreateDirs(q.partner_cache);
                const std::wstring p = q.partner_cache + L"\\" + Wide(tx::Hex(q.id, 32)) + L".sttx";
                if (WriteAtomic(p, blob)) {
                    path = p;
                    PruneDir(q.partner_cache, q.cache_max);   // the same cap as the blobs received over the link
                }
            }
            StoreReceived(q.id, q.r.slot, std::move(blob), path);
            m.ok = true;
        }
    }
    m.ms = static_cast<unsigned>(GetTickCount64() - t0);
    std::snprintf(m.why, sizeof(m.why), "%s", why.c_str());
    (m.ok ? g_made_here : g_made_failed).fetch_add(1);
    PushMade(m);
}

void DrainMakeQueue() {
    ReadyCtx ctx;   // S0 and S1 of one colour: her Original pictures decoded and the colour made once
    {
        AcquireSRWLockShared(&g_lock);
        const bool any = !g_make_queue.empty();
        ReleaseSRWLockShared(&g_lock);
        if (any && ExtractionRunning()) return;   // made from the new Original pictures when it ends (about 10 s)
    }
    for (;;) {
        MakeRequest q;
        bool have = false;
        AcquireSRWLockExclusive(&g_lock);
        if (!g_make_queue.empty()) {
            q = std::move(g_make_queue.front());
            g_make_queue.erase(g_make_queue.begin());
            have = true;
        }
        ReleaseSRWLockExclusive(&g_lock);
        if (!have) return;
        SetState("making");
        try {
            MakeForPartnerNow(q, ctx);
        } catch (...) {
            // out of memory (the request in flight is off the queue): it still gets its result, so the partner's look
            // asks the guest for that blob instead of waiting for it
            ctx = ReadyCtx{};
            Made m;
            std::memcpy(m.id, q.id, 32);
            std::snprintf(m.why, sizeof(m.why), "out of memory while making it");
            g_made_failed.fetch_add(1);
            PushMade(m);
        }
        SetState("idle");
    }
}

// The slot `si` of a painted ("yours") colour folder: each of its pictures from the folder when there, else her Original.
// Left out (false, nothing logged) when every input is her Original's bytes. The blob is stored; `id_out` its id.
bool BuildFromFolder(std::size_t si, const std::string& folder, const char* what, std::string& sig, std::uint8_t id_out[32]) {
    const tx::Slot& s = tx::kSlots[si];
    const std::string orig_prefix = std::string(kSuitDir) + "\\" + kOriginal + "\\";
    std::vector<std::pair<std::wstring, std::string>> comps;
    std::vector<std::string> components;
    bool differs = false, missing = false;
    std::string key = "stx-built|" + std::to_string(tx::kBlobEncoderVersion) + "|" + s.id;
    for (const tx::ManifestRow& r : g_manifest.rows) {
        if (r.kind != "original" || r.slot != s.id || !StartsWithI(r.path, orig_prefix)) continue;
        const std::string name = FileName(r.path);
        const std::wstring user = folder.empty() ? std::wstring() : Dir() + L"\\" + Wide(folder) + L"\\" + Wide(name);
        const std::wstring orig = Dir() + L"\\" + Wide(r.path);
        const std::string usha = user.empty() ? std::string() : FileSha(user);
        const bool use_user = !usha.empty();
        const std::string sha = use_user ? usha : FileSha(orig);
        if (sha.empty()) missing = true;
        if (!use_user && !sha.empty() && sha != r.sha256) {
            Log("[tex] %s changed since the extraction: run Extract textures again (slot %s skipped)", r.path.c_str(), s.id);
            missing = true;
        }
        if (sha != r.sha256) differs = true;
        comps.emplace_back(use_user ? user : orig, use_user ? folder + "\\" + name : r.path);
        components.push_back(r.component);
        key += "|" + r.component + ":" + sha;
    }
    if (!differs || comps.empty()) return false;
    if (missing) {
        Log("[tex] %s: %s pictures are missing: press Extract textures again", what, s.id);
        return false;
    }
    tx::Sha256 kh{};
    tx::HashSha256(key.data(), key.size(), kh);
    const std::string key_hex = tx::Hex(kh);
    sig += key_hex;
    return BuildSlot(static_cast<int>(si), comps, components, key_hex, id_out);
}

// The edited slots of one group (her suit or her boots) from one folder (relative: "Fragile porter suit\16 Gold",
// "Fragile boots\09 Chiral boots\Worn"). A picture there counts as edited when it differs from what the extractor seeded
// in that folder (seed rows under `seed_base` whose folder has the number `seed_num`), or, with no seed (Gold\, Worn\,
// a map the extractor did not seed), from her original. The other maps of an edited slot come from the folder when there,
// else from Original\. `what` names the folder in the log ("colour 16", "boots 9").
void BuildFolder(tx::SlotGroup group, const std::string& folder, const char* seed_base, int seed_num, const char* what, Look& out, std::string& sig,
                 std::string& names) {
    const std::string orig_prefix = std::string(kSuitDir) + "\\" + kOriginal + "\\";
    for (std::size_t si = 0; si < tx::kSlotCount; ++si) {
        const tx::Slot& s = tx::kSlots[si];
        if (s.group != group) continue;
        std::vector<std::pair<std::wstring, std::string>> comps;
        std::vector<std::string> components;
        bool edited = false, missing = false;
        std::string key = "stx-built|" + std::to_string(tx::kBlobEncoderVersion) + "|" + s.id;
        for (const tx::ManifestRow& r : g_manifest.rows) {
            if (r.kind != "original" || r.slot != s.id || !StartsWithI(r.path, orig_prefix)) continue;
            const std::string name = FileName(r.path);
            const std::wstring user = Dir() + L"\\" + Wide(folder) + L"\\" + Wide(name);
            const std::wstring orig = Dir() + L"\\" + Wide(r.path);
            std::string sha = FileSha(user);
            bool use_user = false;
            if (!sha.empty()) {
                // the seed row of this folder (matched by its number: the user may have renamed the rest)
                const tx::ManifestRow* seed = nullptr;
                if (seed_num >= 0)
                    for (const tx::ManifestRow& q : g_manifest.rows)
                        if (q.kind == "seed" && q.slot == s.id && StartsWithI(q.path, std::string(seed_base) + "\\") &&
                            tx::FolderNumber(ParentName(q.path)) == seed_num && tx::SamePath(FileName(q.path), name)) {
                            seed = &q;
                            break;
                        }
                const bool changed = seed ? sha != seed->sha256 : sha != r.sha256;
                if (changed) {
                    use_user = true;
                    edited = true;
                }
            }
            std::string use_sha = use_user ? sha : FileSha(orig);
            if (!use_user && use_sha.empty()) missing = true;
            if (!use_user && !use_sha.empty() && use_sha != r.sha256) {
                Log("[tex] %s changed since the extraction: run Extract textures again (slot %s skipped)", r.path.c_str(), s.id);
                missing = true;
            }
            comps.emplace_back(use_user ? user : orig, use_user ? folder + "\\" + name : r.path);
            components.push_back(r.component);
            key += "|" + r.component + ":" + use_sha;
        }
        if (!edited) continue;
        if (missing || comps.empty()) {
            Log("[tex] %s: %s is edited but %s pictures are missing: press Extract textures again", what, s.id, s.id);
            continue;
        }
        tx::Sha256 kh{};
        tx::HashSha256(key.data(), key.size(), kh);
        const std::string key_hex = tx::Hex(kh);
        sig += key_hex;
        std::uint8_t id[32];
        if (!BuildSlot(static_cast<int>(si), comps, components, key_hex, id)) continue;
        out.slots.push_back(static_cast<int>(si));
        out.blob_ids.insert(out.blob_ids.end(), id, id + 32);
        names += (names.empty() ? "" : " ") + std::string(s.id);
    }
}

// Round 12 (docs/real-partner-body.md section 37.5): the palette row her raised hood is drawn in for a suit slot S0 built from
// files: the Colour picture S0 uses (the folder's when there, else her Original) averaged over her navy fabric (the
// ready-made generator's mask, from her Original), then the nearest row (suit_colour_math NearestRow: her own navy is row 0).
// -1 (logged) when it cannot be measured (no analysis, another picture size, too little fabric): the carrier keeps Sam's look.
int PaintedRow(int colour, const std::string& folder, ReadyCtx& ctx) {
    std::string why;
    const std::string key = OriginalsKey(&why);
    if (key.empty() || !EnsureAnalysis(key, ctx.oi, nullptr, &why)) {
        Log("[tex] colour %d: her hood's colour row not measured (%s)", colour, why.c_str());
        return -1;
    }
    const char* name = rm::kPictureNames[rm::kColourPic];
    tx::Image img;
    const tx::Image* c = nullptr;
    std::string err;
    const std::wstring user = folder.empty() ? std::wstring() : Dir() + L"\\" + Wide(folder) + L"\\" + Wide(name);
    const bool from_folder = !user.empty() && !FileSha(user).empty();
    if (from_folder) {
        if (tx::ReadPngFile(user, img, &err)) c = &img;
    } else {
        c = OrigImage(ctx.oi, kOrigNames[0], &err);
    }
    const std::size_t px = static_cast<std::size_t>(g_an.w) * g_an.h;
    if (!c || c->w != g_an.w || c->h != g_an.h || g_an.mask.size() != px || c->rgba.size() != px * 4) {
        Log("[tex] colour %d: her hood's colour row not measured (%s %s)", colour, name, c ? "is not the size of her Original" : err.c_str());
        return -1;
    }
    std::uint64_t n = 0;
    st::suitcol::Rgb avg{};
    if (!st::suitcol::FabricAverage(c->rgba.data(), g_an.mask.data(), px, &avg, &n)) {
        Log("[tex] colour %d: her hood's colour row not measured (too little fabric: %llu pixels)", colour, static_cast<unsigned long long>(n));
        return -1;
    }
    const int row = st::suitcol::NearestRow(avg);
    Log("[tex] colour %d: her suit's fabric (%s) averages (%u, %u, %u): her hood in palette row %d", colour, from_folder ? "the folder's Colour" : "her Original",
        avg.r, avg.g, avg.b, row);
    return row;
}

// Round 8: a colour folder 01..16 with ready-made colours on (ready_made_rules.hpp): nothing painted, the ready-made
// colour made from Original\; something painted (any of the six pictures), exactly the folder's pictures. A slot of a
// painted folder with no edited picture is the ready-made slot only when the two blobs are byte for byte the same.
bool BuildColourReadyMade(int colour, const std::string& folder, Look& out, std::string& sig_out) {
    rm::PictureState st[rm::kPictures];
    std::string sig = "rm|" + std::to_string(df::kGeneratorVersion) + "|" + std::to_string(colour >= 15 ? g_variant.load() : 0);
    for (int i = 0; i < rm::kPictures; ++i) {
        st[i] = rm::PictureState::missing;
        if (folder.empty()) continue;
        const std::string sha = FileSha(Dir() + L"\\" + Wide(folder) + L"\\" + Wide(rm::kPictureNames[i]));
        sig += "|" + sha;   // every picture the mod reads: painting any of them rebuilds the look
        if (sha.empty()) continue;
        // the seed row of this folder, matched by its number (the user may have renamed the rest)
        const tx::ManifestRow* seed = nullptr;
        for (const tx::ManifestRow& q : g_manifest.rows)
            if (q.kind == "seed" && StartsWithI(q.path, std::string(kSuitDir) + "\\") && tx::FolderNumber(ParentName(q.path)) == colour &&
                tx::SamePath(FileName(q.path), rm::kPictureNames[i])) {
                seed = &q;
                break;
            }
        const tx::ManifestRow* orig = g_manifest.Find(OrigRel(rm::kPictureNames[i]));
        const bool unedited = seed ? sha == seed->sha256 : (orig && sha == orig->sha256);
        st[i] = unedited ? rm::PictureState::unedited : rm::PictureState::edited;
    }
    const rm::FolderState fs = rm::Classify(st);
    std::string names_files, names_ready;
    int n_files = 0;
    char what[32];
    std::snprintf(what, sizeof(what), "colour %d", colour);
    ReadyCtx ctx;   // her Original pictures decoded once for S0, S1 and their identity
    bool s0_in = false, s0_files = false;   // round 12: S0 in the look; built from the folder's files (not the ready-made slot)
    for (std::size_t si = 0; si < tx::kSlotCount; ++si) {
        const tx::Slot& s = tx::kSlots[si];
        if (s.group != tx::SlotGroup::suit) continue;
        const int k = static_cast<int>(si);
        const rm::SlotSource src = rm::SourceFor(colour, k, fs, true, st);
        std::uint8_t id[32];
        bool ready = false;
        if (src == rm::SlotSource::ready_made) {
            if (!BuildDefaultSlot(k, colour, id, ctx)) continue;
            ready = true;
        } else if (src == rm::SlotSource::files) {
            if (!BuildFromFolder(si, folder, what, sig, id)) continue;   // left out: all her Original (her own)
            if (rm::MayBeReadyMade(colour, k, fs, true, st)) {
                // the folder's unedited pictures of this slot: the ready-made slot when the blobs are the same (sent as a
                // recipe); otherwise (an older extraction's pictures, another generator) the files as they are
                std::uint8_t rid[32];
                ready = BuildDefaultSlot(k, colour, rid, ctx, false) && std::memcmp(rid, id, 32) == 0;
            }
        } else {
            continue;
        }
        out.slots.push_back(k);
        out.blob_ids.insert(out.blob_ids.end(), id, id + 32);
        if (k == rm::kSlotS0) s0_in = true, s0_files = !ready;
        if (ready) {
            names_ready += (names_ready.empty() ? "" : " ") + std::string(s.id);
            sig += "|rm" + tx::Hex(id, 32);
        } else {
            names_files += (names_files.empty() ? "" : " ") + std::string(s.id);
            ++n_files;
        }
    }
    LookId(out);
    sig_out = sig;
    // round 12 (section 37.5): her hood's colour row: her own fabric (no S0) 0, the ready-made S0 the folder's; a painted S0 its
    // measured row (a silver / gold folder whose Colour picture is not painted keeps its metal)
    const bool measure = st::suitcol::MeasureHoodRow(colour, s0_files, st[rm::kColourPic] == rm::PictureState::edited);
    out.hood_row = st::suitcol::ReadyMadeLookRow(colour, s0_in, measure ? PaintedRow(colour, folder, ctx) : -1);
    std::string how;
    if (fs == rm::FolderState::ready_made) {
        how = names_ready.empty() ? "ready-made colour not available (her own suit)" : "ready-made (" + names_ready + ")";
    } else {
        how = "yours: " + std::to_string(n_files) + " slot(s)" +
              (names_files.empty() ? std::string() : " (" + names_files + ")") + (names_ready.empty() ? std::string() : ", ready-made " + names_ready);
    }
    Log("[tex] look %s for colour %d (%s): %s", out.Empty() ? "--------" : tx::Hex(out.id, 4).c_str(), colour, folder.empty() ? "no folder" : folder.c_str(),
        how.c_str());
    return true;
}

// One colour's look from the files. False when the folder cannot be read (the look stays unsettled).
bool BuildColour(int colour, Look& out, std::string& sig_out) {
    out = Look{};
    out.colour = colour;
    const std::string folder = ColourFolder(colour);
    if (g_cfg.defaults && df::HasDefault(colour)) return BuildColourReadyMade(colour, folder, out, sig_out);
    if (folder.empty()) {
        sig_out = "none";
        out.hood_row = 0;
        return true;  // no folder: her own look
    }
    std::string sig, names;
    char what[32];
    std::snprintf(what, sizeof(what), "colour %d", colour);
    BuildFolder(tx::SlotGroup::suit, folder, kSuitDir, colour, what, out, sig, names);
    LookId(out);
    sig_out = sig.empty() ? "none" : sig;
    // round 12 (section 37.5): her hood's colour row (her navy 0; a painted S0 its measured row)
    const bool s0_in = std::find(out.slots.begin(), out.slots.end(), rm::kSlotS0) != out.slots.end();
    ReadyCtx ctx;
    out.hood_row = st::suitcol::FolderLookRow(s0_in, s0_in ? PaintedRow(colour, folder, ctx) : -1);
    Log("[tex] look %s for colour %d (%s): %zu slot(s) edited%s%s%s", out.Empty() ? "--------" : tx::Hex(out.id, 4).c_str(), colour, folder.c_str(),
        out.slots.size(), names.empty() ? "" : " (", names.c_str(), names.empty() ? "" : ")");
    return true;
}

// The boots folder of a boot type (matched by its "NN " prefix), relative ("Fragile boots\09 Chiral boots"), or "".
std::string BootsFolder(int number) {
    const std::wstring base = Dir() + L"\\" + Wide(kBootsDir);
    WIN32_FIND_DATAW fd{};
    const HANDLE h = FindFirstFileW((base + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::string found;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        const std::string name = Utf8(fd.cFileName);
        if (tx::FolderNumber(name) == number) {
            found = std::string(kBootsDir) + "\\" + name;
            break;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

// One boots look (bootslook::Key: the type's folder, worn, metal): the first of Worn\ (worn), Gold\ / Silver\ (the gold /
// silver suit) and the type's folder itself that holds an edited picture. Empty: her own boots.
bool BuildBoots(int key, Look& out, std::string& sig_out) {
    out = Look{};
    int number = 0, metal = 0;
    bool worn = false;
    bootslook::Unkey(key, &number, &worn, &metal);
    const std::string folder = BootsFolder(number);
    sig_out = "none";
    if (folder.empty()) {
        Log("[tex] boots look -------- for boots %d: no \"%s\\%02d ...\" folder", number, kBootsDir, number);
        return true;
    }
    const char* subs[bootslook::kMaxCandidates] = {};
    const int n = bootslook::Candidates(worn, metal, subs);
    std::string used;
    std::string sig, names;
    for (int i = 0; i < n && out.Empty(); ++i) {
        const std::string f = subs[i][0] ? folder + "\\" + subs[i] : folder;
        char what[48];
        std::snprintf(what, sizeof(what), "boots %d%s%s", number, subs[i][0] ? " " : "", subs[i]);
        sig.clear();
        names.clear();
        // the type's folder has its seeds; Gold\ Silver\ Worn\ are compared with her originals
        BuildFolder(tx::SlotGroup::boots, f, kBootsDir, subs[i][0] ? -1 : number, what, out, sig, names);
        if (!out.Empty()) used = f;
    }
    LookId(out);
    sig_out = sig.empty() ? "none" : sig;
    Log("[tex] boots look %s for boots %d%s%s: %s%s%s%s", out.Empty() ? "--------" : tx::Hex(out.id, 4).c_str(), number, worn ? " worn" : "",
        metal == 1 ? " gold" : metal == 2 ? " silver" : "", out.Empty() ? "nothing edited (her own boots)" : used.c_str(), names.empty() ? "" : " (",
        names.c_str(), names.empty() ? "" : ")");
    return true;
}

// ---------------------------------------------------------------- test patterns (generated pixels only)
void BuildPattern(int pattern) {
    Look l;
    l.colour = -2;
    // S0: magenta / green 256-px checker, a bright yellow diagonal band (orientation), roughness 0.5 (not in pattern 3)
    if (pattern != 3) {
        const tx::Slot& s = tx::kSlots[0];
        tx::Image img;
        img.Resize(s.w, s.h);
        for (std::uint32_t y = 0; y < s.h; ++y)
            for (std::uint32_t x = 0; x < s.w; ++x) {
                std::uint8_t* p = img.At(x, y);
                const bool c = ((x / 128) ^ (y / 128)) & 1;
                const bool band = ((x + y) / 64) % 8 == 0;
                p[0] = band ? 255 : c ? 230 : 20;
                p[1] = band ? 230 : c ? 20 : 220;
                p[2] = band ? 0 : c ? 200 : 40;
                p[3] = 128;
            }
        std::vector<std::uint8_t> blob;
        std::string why;
        if (!tx::BuildBlob(s, img, blob, &why)) {
            Log("[tex] pattern: %s", why.c_str());
            return;
        }
        tx::Sha256 h{};
        tx::HashSha256(blob.data(), blob.size(), h);
        StoreBlob(h.data(), 0, std::move(blob));
        l.slots.push_back(0);
        l.blob_ids.insert(l.blob_ids.end(), h.begin(), h.end());
    }
    if (pattern >= 2) {
        // B1 (her boots' colour, for the boots rule): yellow / black stripes (pattern 2: with the suit checker, 3: alone)
        const int bi = tx::SlotIndex("B1");
        const tx::Slot& s = tx::kSlots[bi];
        tx::Image img;
        img.Resize(s.w, s.h);
        for (std::uint32_t y = 0; y < s.h; ++y)
            for (std::uint32_t x = 0; x < s.w; ++x) {
                std::uint8_t* p = img.At(x, y);
                const bool st = ((x + y) / 48) & 1;
                p[0] = st ? 250 : 10;
                p[1] = st ? 220 : 10;
                p[2] = st ? 0 : 10;
                p[3] = 255;
            }
        std::vector<std::uint8_t> blob;
        std::string why;
        if (tx::BuildBlob(s, img, blob, &why)) {
            tx::Sha256 h{};
            tx::HashSha256(blob.data(), blob.size(), h);
            StoreBlob(h.data(), bi, std::move(blob));
            l.slots.push_back(bi);
            l.blob_ids.insert(l.blob_ids.end(), h.begin(), h.end());
        }
    }
    LookId(l);
    AcquireSRWLockExclusive(&g_lock);
    const std::string lid = tx::Hex(l.id, 4);
    const std::size_t lslots = l.slots.size();
    g_pattern_look = std::move(l);  // no allocation under the lock
    g_pattern_built = pattern;
    ReleaseSRWLockExclusive(&g_lock);
    Log("[tex] pattern %d built: look %s (%zu slot(s))", pattern, lid.c_str(), lslots);
}

// ---------------------------------------------------------------- the Fragile Express logo (round 20)
// The copies of Sam's hood maps and the Bridges cap's maps with BRIDGES replaced by FRAGILE express (logo_recipe.hpp),
// made from the extracted reference pictures (each must be the extractor's own bytes: the manifest's SHA-256) and cached
// in the built cache like any built slot. Made once per manifest (a new one, a re-extract, makes them again: review r20 visual
// 7); a failure is logged once and waits for a new manifest.
std::atomic<bool> g_logo_wanted{false};
Look g_logo_looks[3];            // g_lock: hood palette, hood metal, cap (empty: not made)
bool g_logo_made = false;        // g_lock
FileStamp g_logo_made_stamp{};   // worker: the manifest the made copies come from
FileStamp g_logo_tried_stamp{};  // worker: the manifest the last attempt read
bool g_logo_tried = false;

constexpr char kLogoDecal[] = "Logo (reference)\\Fragile decal - Alpha.png";
constexpr char kHoodPal[] = "Sam porter suit\\Palette colours (00-14)\\Suit - ";
constexpr char kHoodMetal[] = "Sam porter suit\\15 Silver\\Suit - ";
constexpr char kHoodGold[] = "Sam porter suit\\16 Gold\\Suit - ";
constexpr char kCapDir[] = "Logo (reference)\\";

// One reference picture: its manifest row (the extractor wrote it), its bytes unchanged since, decoded at the row's size.
bool LogoPicture(const std::string& rel, tx::Image& out, std::string& sig, std::string* why) {
    const tx::ManifestRow* row = nullptr;
    for (const tx::ManifestRow& r : g_manifest.rows)
        if (r.path.size() == rel.size() && StartsWithI(r.path, rel)) row = &r;
    if (!row) return *why = "no " + rel + " in the extraction (press Extract textures)", false;
    const std::wstring path = Dir() + L"\\" + Wide(rel);
    const std::string sha = FileSha(path);
    if (sha.empty()) return *why = rel + " is missing", false;
    if (sha != row->sha256) return *why = rel + " is not the extractor's picture (edited?)", false;
    std::vector<std::uint8_t> b;
    std::string err;
    if (!ReadAll(path, b) || !tx::DecodePicture(b.data(), b.size(), out, 2048, 2048, nullptr, &err) || out.w != row->w || out.h != row->h)
        return *why = rel + " does not read (" + err + ")", false;
    sig += sha;
    return true;
}

// A blank packed picture (alpha 255) with `map`'s first channels in `channels`.
tx::Image Packed(const tx::Image& map, const std::vector<int>& channels) {
    tx::Image p;
    p.Resize(map.w, map.h);
    for (std::size_t i = 3; i < p.rgba.size(); i += 4) p.rgba[i] = 255;
    tx::InsertChannels(p, map, channels);
    return p;
}

// One logo slot's blob from the cache or `packed` (null: the cache only); stored, its id appended to the look.
bool LogoBlob(int li, const tx::Image* packed, const std::string& sig, Look& look) {
    const int slot = tx::kLogoBase + li;
    const tx::Slot& s = tx::kLogoSlots[li];
    char kt[128];
    std::snprintf(kt, sizeof(kt), "stx-logo|%d|%u|%s|", tx::logo::kRecipeVersion, tx::kBlobEncoderVersion, s.id);
    const std::string key = std::string(kt) + sig;
    tx::Sha256 kh{};
    tx::HashSha256(key.data(), key.size(), kh);
    const std::wstring cache_dir = CacheDir();
    const std::wstring cache_file = cache_dir.empty() ? std::wstring() : cache_dir + L"\\" + Wide(tx::Hex(kh)) + L".sttx";
    std::vector<std::uint8_t> blob;
    std::string why;
    bool cached = !cache_file.empty() && ReadAll(cache_file, blob) && tx::ValidateBlob(s, blob.data(), blob.size(), nullptr, &why);
    if (!cached) {
        if (!packed) return false;
        blob.clear();
        if (!tx::BuildBlob(s, *packed, blob, &why)) {
            Log("[logo] %s: the blob was not built (%s)", s.id, why.c_str());
            return false;
        }
        if (!cache_file.empty()) {
            CreateDirs(cache_dir);
            if (WriteAtomic(cache_file, blob)) {
                cached = true;
                PruneBuiltCache();
            }
        }
        g_built.fetch_add(1);
    }
    tx::Sha256 h{};
    tx::HashSha256(blob.data(), blob.size(), h);
    look.slots.push_back(slot);
    look.blob_ids.insert(look.blob_ids.end(), h.begin(), h.end());
    StoreBlob(h.data(), slot, std::move(blob), cached ? cache_file : std::wstring());
    return true;
}

// The three logo looks: from the cache when every blob is there, else made from the pictures. Each source picture is freed as
// soon as its packed copy exists (review r20 visual 6): at the peak about six decoded 2048 x 2048 RGBA pictures (16 MB each:
// the decal, the palette mask kept for the metal set, the three packed hood maps and one source), about 100 MB.
bool BuildLogo(std::string* why) {
    const ULONGLONG t0 = GetTickCount64();
    // the inputs' identity first (hashes only): a full cache needs no decoding
    std::string sig;
    for (const std::string& rel : {std::string(kLogoDecal), std::string(kHoodPal) + "Colour.png", std::string(kHoodPal) + "Mask alpha.png",
                                   std::string(kHoodPal) + "Roughness.png", std::string(kHoodPal) + "Normal.png", std::string(kHoodMetal) + "Colour.png",
                                   std::string(kHoodMetal) + "Mask alpha.png", std::string(kHoodMetal) + "Roughness.png", std::string(kHoodMetal) + "Reflectance.png",
                                   std::string(kHoodMetal) + "Ambient occlusion.png", std::string(kHoodGold) + "Reflectance.png",
                                   std::string(kHoodGold) + "Ambient occlusion.png", std::string(kCapDir) + "Cap - Colour.png",
                                   std::string(kCapDir) + "Cap - Mask alpha.png", std::string(kCapDir) + "Cap - Normal.png"}) {
        const tx::ManifestRow* row = nullptr;
        for (const tx::ManifestRow& r : g_manifest.rows)
            if (r.path.size() == rel.size() && StartsWithI(r.path, rel)) row = &r;
        if (!row) return *why = "no " + rel + " in the extraction (an older extraction: press Extract textures)", false;
        sig += row->sha256;
    }
    tx::Sha256 sh{};
    tx::HashSha256(sig.data(), sig.size(), sh);
    const std::string isig = tx::Hex(sh);
    Look looks[3];
    // the normal map's content is the same in every suit set: both hood looks bind L4. The cap: its palette set (v00, the
    // colours 0..22) only; the silver / gold sets keep BRIDGES (their own normal and reflectance / roughness maps carry it
    // too: a colour copy alone left a dark BRIDGES and hid the new word, round 20's solo stills).
    const int parts[3][5] = {{tx::kHoodColour, tx::kHoodMask, tx::kHoodNormal, -1, -1},
                             {tx::kMetalColour, tx::kMetalMask, tx::kHoodNormal, tx::kSilverRefl, tx::kGoldRefl},
                             {tx::kCapColour, tx::kCapNormal, -1, -1, -1}};
    bool all_cached = true;
    for (int p = 0; p < 3 && all_cached; ++p)
        for (int k = 0; k < 5 && parts[p][k] >= 0 && all_cached; ++k) all_cached = LogoBlob(parts[p][k], nullptr, isig, looks[p]);
    if (!all_cached) {
        for (Look& l : looks) l = Look{};
        std::string s2;   // the pictures checked against the manifest as they are read
        tx::Image decal, pal_mask;
        if (!LogoPicture(kLogoDecal, decal, s2, why)) return false;
        {   // the palette hood (with the normal map, shared by every set); each source freed once packed
            tx::Image mask, nrm, col;
            {
                tx::Image ma, rough;
                if (!LogoPicture(std::string(kHoodPal) + "Mask alpha.png", ma, s2, why) || !LogoPicture(std::string(kHoodPal) + "Roughness.png", rough, s2, why))
                    return false;
                mask = Packed(ma, {0});
                tx::InsertChannels(mask, rough, {1});
            }
            pal_mask = mask;
            {
                tx::Image normal;
                if (!LogoPicture(std::string(kHoodPal) + "Normal.png", normal, s2, why)) return false;
                nrm = Packed(normal, {0, 1});
            }
            {
                tx::Image colour;
                if (!LogoPicture(std::string(kHoodPal) + "Colour.png", colour, s2, why)) return false;
                col = Packed(colour, {0, 1, 2});
            }
            if (!tx::logo::StampHood(&col, &mask, &nrm, pal_mask, decal, why)) return *why = "the palette hood: " + *why, false;
            if (!LogoBlob(tx::kHoodColour, &col, isig, looks[0]) || !LogoBlob(tx::kHoodMask, &mask, isig, looks[0]) || !LogoBlob(tx::kHoodNormal, &nrm, isig, looks[0]))
                return *why = "a palette hood blob was not built", false;
        }
        {   // the metal hood (silver and gold share these two textures); each source freed once packed
            tx::Image mask, col;
            {
                tx::Image ma, rough;
                if (!LogoPicture(std::string(kHoodMetal) + "Mask alpha.png", ma, s2, why) || !LogoPicture(std::string(kHoodMetal) + "Roughness.png", rough, s2, why))
                    return false;
                mask = Packed(ma, {0});
                tx::InsertChannels(mask, rough, {1});
            }
            {
                tx::Image colour;
                if (!LogoPicture(std::string(kHoodMetal) + "Colour.png", colour, s2, why)) return false;
                col = Packed(colour, {0, 1, 2});
            }
            if (!tx::logo::StampHood(&col, &mask, nullptr, pal_mask, decal, why)) return *why = "the metal hood: " + *why, false;
            if (!LogoBlob(tx::kMetalColour, &col, isig, looks[1]) || !LogoBlob(tx::kMetalMask, &mask, isig, looks[1])) return *why = "a metal hood blob was not built", false;
            // L4 (the last of the palette look) as made above
            looks[1].slots.push_back(looks[0].slots.back());
            looks[1].blob_ids.insert(looks[1].blob_ids.end(), looks[0].blob_ids.end() - 32, looks[0].blob_ids.end());
        }
        for (const char* set : {kHoodMetal, kHoodGold}) {   // the metal hoods' reflectance (silver, gold: their own)
            tx::Image refl, ao;
            if (!LogoPicture(std::string(set) + "Reflectance.png", refl, s2, why) || !LogoPicture(std::string(set) + "Ambient occlusion.png", ao, s2, why))
                return false;
            tx::Image p = Packed(refl, {0, 1, 2});
            tx::InsertChannels(p, ao, {3});
            if (!tx::logo::StampHoodReflectance(&p, pal_mask, decal, why)) return *why = "the metal hood's reflectance: " + *why, false;
            if (!LogoBlob(set == kHoodGold ? tx::kGoldRefl : tx::kSilverRefl, &p, isig, looks[1])) return *why = "a metal hood reflectance blob was not built", false;
        }
        std::vector<std::uint8_t>().swap(pal_mask.rgba);
        {   // the cap: its palette set (v00: colour + mask alpha, and its normal map)
            tx::Image c0, a0, n0;
            if (!LogoPicture(std::string(kCapDir) + "Cap - Colour.png", c0, s2, why) || !LogoPicture(std::string(kCapDir) + "Cap - Mask alpha.png", a0, s2, why) ||
                !LogoPicture(std::string(kCapDir) + "Cap - Normal.png", n0, s2, why))
                return false;
            tx::Image p0 = Packed(c0, {0, 1, 2});
            tx::InsertChannels(p0, a0, {3});
            tx::Image nrm = Packed(n0, {0, 1});
            if (!tx::logo::StampCap(&p0, true, &nrm, a0, decal, why)) return *why = "the cap: " + *why, false;
            if (!LogoBlob(tx::kCapColour, &p0, isig, looks[2]) || !LogoBlob(tx::kCapNormal, &nrm, isig, looks[2])) return *why = "a cap blob was not built", false;
        }
    }
    for (Look& l : looks) LookId(l);
    char ids[3][12];
    for (int p = 0; p < 3; ++p) std::snprintf(ids[p], sizeof(ids[p]), "%s", tx::Hex(looks[p].id, 4).c_str());
    AcquireSRWLockExclusive(&g_lock);
    for (int p = 0; p < 3; ++p) g_logo_looks[p] = std::move(looks[p]);
    g_logo_made = true;
    ReleaseSRWLockExclusive(&g_lock);
    Log("[logo] the Fragile Express logo copies %s in %llu ms: hood palette %s, hood metal %s, cap %s (recipe %d)", all_cached ? "from the cache" : "made",
        static_cast<unsigned long long>(GetTickCount64() - t0), ids[0], ids[1], ids[2], tx::logo::kRecipeVersion);
    return true;
}

void LogoTick() {
    if (!g_logo_wanted.load() || g_cfg.dir.empty()) return;
    AcquireSRWLockShared(&g_lock);
    const bool made = g_logo_made;
    ReleaseSRWLockShared(&g_lock);
    if (ExtractionRunning() || !LoadManifest()) return;
    // made from this manifest: done (a new manifest, a re-extract, makes them again; the old copies stay until then)
    if (made && g_logo_made_stamp.size == g_manifest_stamp.size && g_logo_made_stamp.mtime == g_manifest_stamp.mtime) return;
    if (g_logo_tried && g_logo_tried_stamp.size == g_manifest_stamp.size && g_logo_tried_stamp.mtime == g_manifest_stamp.mtime) return;
    g_logo_tried = true;
    g_logo_tried_stamp = g_manifest_stamp;
    SetState("logo");
    std::string why;
    if (BuildLogo(&why)) g_logo_made_stamp = g_manifest_stamp;
    else Log("[logo] the logo copies were not made: %s (her hood and cap %s)", why.c_str(), made ? "keep the copies made before" : "keep BRIDGES");
    SetState("idle");
}

// ---------------------------------------------------------------- the worker
void Worker() {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    HANDLE watch = INVALID_HANDLE_VALUE;
    ULONGLONG changed_at = 0;
    bool dirty = true;
    for (;;) {
        try {
            const int pat = g_pattern.load();
            if (pat && pat != g_pattern_built) BuildPattern(pat);
            DrainMakeQueue();   // host: the guest's ready-made blobs, made here (round 8)
            LogoTick();         // round 20: host and guest, once wanted
            if (!g_cfg.dir.empty() && g_cfg.guest) {
                if (watch == INVALID_HANDLE_VALUE && g_cfg.hot_reload)
                    watch = FindFirstChangeNotificationW(g_cfg.dir.c_str(), TRUE,
                                                         FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE |
                                                             FILE_NOTIFY_CHANGE_DIR_NAME);
                if (watch != INVALID_HANDLE_VALUE && WaitForSingleObject(watch, 0) == WAIT_OBJECT_0) {
                    changed_at = GetTickCount64();
                    FindNextChangeNotification(watch);
                }
                if (changed_at && GetTickCount64() - changed_at >= 1000) {
                    changed_at = 0;
                    dirty = true;
                }
                if (g_rescan.exchange(false)) dirty = true;
                if (dirty) {  // every built look is stale: rebuilt when wanted, the old one answers until the new one is built
                    AcquireSRWLockExclusive(&g_lock);
                    for (ColourLook& x : g_looks) x.stale = x.settled;
                    for (ColourLook& x : g_boots_looks) x.stale = x.settled;
                    ReleaseSRWLockExclusive(&g_lock);
                    dirty = false;
                }
                const int c = g_wanted.load();
                const int bk = g_boots_wanted.load();
                const bool boots_ok = bk >= 0 && bk < bootslook::kKeyCount;
                bool need = false, need_boots = false;
                if (c >= 0 && c <= 16) {
                    AcquireSRWLockShared(&g_lock);
                    need = !g_looks[c].settled || g_looks[c].stale;
                    need_boots = boots_ok && (!g_boots_looks[bk].settled || g_boots_looks[bk].stale);
                    ReleaseSRWLockShared(&g_lock);
                }
                if ((need || need_boots) && c >= 0 && c <= 16 && ExtractionRunning()) {
                    // the launcher is extracting: the old look answers until it ends, then this is built from the new index
                } else if ((need || need_boots) && c >= 0 && c <= 16 && LoadManifest()) {
                    SetState("building");
                    Look l;
                    std::string sig;
                    if (need && BuildColour(c, l, sig)) {
                        const bool empty = l.Empty();
                        const std::string id = tx::Hex(l.id, 4);
                        AcquireSRWLockExclusive(&g_lock);
                        const bool changed = g_looks[c].sig.size() && g_looks[c].sig != sig;
                        g_looks[c].settled = true;
                        g_looks[c].stale = false;
                        g_looks[c].look = std::move(l);  // swapped in whole: no allocation under the lock
                        g_looks[c].sig = std::move(sig);
                        ReleaseSRWLockExclusive(&g_lock);
                        if (changed) Log("[tex] reload: colour %d's pictures changed: look %s", c, empty ? "her own" : id.c_str());
                    }
                    if (need_boots && BuildBoots(bk, l, sig)) {
                        const bool empty = l.Empty();
                        const std::string id = tx::Hex(l.id, 4);
                        AcquireSRWLockExclusive(&g_lock);
                        const bool changed = g_boots_looks[bk].sig.size() && g_boots_looks[bk].sig != sig;
                        g_boots_looks[bk].settled = true;
                        g_boots_looks[bk].stale = false;
                        g_boots_looks[bk].look = std::move(l);
                        g_boots_looks[bk].sig = std::move(sig);
                        ReleaseSRWLockExclusive(&g_lock);
                        if (changed) Log("[tex] reload: the boots pictures changed: boots look %s", empty ? "her own" : id.c_str());
                    }
                    SetState("idle");
                } else if ((need || need_boots) && c >= 0 && c <= 16) {
                    // no manifest: her own look (settled empty) so the tint fallback may apply
                    Look own;
                    own.colour = c;
                    AcquireSRWLockExclusive(&g_lock);
                    g_looks[c].settled = true;
                    g_looks[c].stale = false;
                    g_looks[c].look = std::move(own);
                    g_looks[c].sig = "no manifest";
                    if (boots_ok) {
                        g_boots_looks[bk].settled = true;
                        g_boots_looks[bk].stale = false;
                        g_boots_looks[bk].look = Look{};
                        g_boots_looks[bk].sig = "no manifest";
                    }
                    ReleaseSRWLockExclusive(&g_lock);
                }
            }
        } catch (...) {
            // out of memory (or another C++ exception) while building: never the game's end. The wanted looks become
            // her own look until the folder changes again; the lock is never held where something can throw.
            Log("[tex] building her textures failed (out of memory?): her own look until the pictures change again");
            const int c = g_wanted.load();
            const int bk = g_boots_wanted.load();
            AcquireSRWLockExclusive(&g_lock);
            if (c >= 0 && c <= 16) {
                g_looks[c].settled = true;
                g_looks[c].stale = false;
                g_looks[c].look.slots.clear();
                g_looks[c].look.blob_ids.clear();
                std::memset(g_looks[c].look.id, 0, 32);
            }
            if (bk >= 0 && bk < bootslook::kKeyCount) {
                g_boots_looks[bk].settled = true;
                g_boots_looks[bk].stale = false;
                g_boots_looks[bk].look.slots.clear();
                g_boots_looks[bk].look.blob_ids.clear();
                std::memset(g_boots_looks[bk].look.id, 0, 32);
            }
            ReleaseSRWLockExclusive(&g_lock);
            SetState("failed");
            // host requests still queued fail (sent over the link instead): each gets its result (DrainMakeQueue gives the
            // one in flight its own)
            std::vector<MakeRequest> failed;
            AcquireSRWLockExclusive(&g_lock);
            failed.swap(g_make_queue);
            ReleaseSRWLockExclusive(&g_lock);
            for (const MakeRequest& q : failed) {
                Made m;
                std::memcpy(m.id, q.id, 32);
                std::snprintf(m.why, sizeof(m.why), "out of memory");
                g_made_failed.fetch_add(1);
                PushMade(m);
            }
        }
        WaitForSingleObject(g_wake, 250);
    }
    // never reached (the worker lives for the process); kept for symmetry
    // if (watch != INVALID_HANDLE_VALUE) FindCloseChangeNotification(watch);
    // if (SUCCEEDED(co)) CoUninitialize();
    (void)co;
}

void StartWorker() {
    if (g_started) return;
    g_started = true;
    g_made.reserve(64);   // PushMade never allocates under the lock in practice
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::thread(Worker).detach();
}
void Wake() {
    if (g_wake) SetEvent(g_wake);
}

}  // namespace

void Configure(const Settings& s) noexcept {
    g_cfg = s;
    g_variant.store(s.defaults_variant >= 0 && s.defaults_variant < df::kMetalVariants ? s.defaults_variant : 0);
    // the host too (round 8): it makes the guest's ready-made colours from its own Original pictures
    if (s.on && (!s.dir.empty() || g_pattern.load())) StartWorker();
    if (s.on && !s.defaults) Log("[tex] ready-made colours off (session.ini textures_defaults=0): an unpainted colour folder is her own suit");
}

void SetWantedColour(int colour) noexcept {
    if (g_wanted.exchange(colour) != colour) Wake();
}
void SetWantedBoots(int key) noexcept {
    const int k = key >= 0 && key < bootslook::kKeyCount ? key : -1;
    if (g_boots_wanted.exchange(k) != k) Wake();
}

bool RecipeFor(const std::uint8_t id[32], Recipe* out) noexcept {
    bool ok = false;
    const std::string h = tx::Hex(id, 32);
    AcquireSRWLockShared(&g_lock);
    auto it = g_recipes.find(h);
    if (it != g_recipes.end() && it->second.valid) {
        *out = it->second;
        ok = true;
    }
    ReleaseSRWLockShared(&g_lock);
    return ok;
}

void SetLogoWanted(bool on) noexcept {
    if (g_logo_wanted.exchange(on) != on && on) Wake();
}

bool LogoLook(LogoPart part, Look* out) noexcept {
    Look a, b;
    bool made = false;
    try {
        AcquireSRWLockShared(&g_lock);
        made = g_logo_made;
        if (made) {
            a = g_logo_looks[part == LogoPart::hood_metal ? 1 : part == LogoPart::cap ? 2 : 0];
            if (part == LogoPart::hood_any) b = g_logo_looks[1];
        }
        ReleaseSRWLockShared(&g_lock);   // nothing below throws while it is held
    } catch (...) {
        ReleaseSRWLockShared(&g_lock);
        return false;
    }
    if (!made || a.Empty()) return false;
    if (!b.slots.empty()) {   // hood_any: L0 L1 L4 and L2 L3 L9 L10 (L4 once), sorted by slot (the look id's order; tx::MergeLookRows)
        try {
            Look l;
            for (const auto& r : tx::MergeLookRows(a.slots, b.slots)) {
                const std::uint8_t* id = r.second < a.slots.size() ? a.blob_ids.data() + 32 * r.second : b.blob_ids.data() + 32 * (r.second - a.slots.size());
                l.slots.push_back(r.first);
                l.blob_ids.insert(l.blob_ids.end(), id, id + 32);
            }
            LookId(l);
            a = std::move(l);
        } catch (...) {
            return false;
        }
    }
    *out = std::move(a);
    return true;
}

bool CanMake() noexcept { return g_cfg.on && !g_cfg.dir.empty() && g_started && g_make_mode.load() != 1; }
int MakeMode() noexcept { return g_make_mode.load(); }
void SetMakeMode(int m) noexcept { g_make_mode.store(m >= 0 && m <= 4 ? m : 0); }

bool MakeForPartner(const Recipe& r, const std::uint8_t id[32], const std::wstring& partner_cache, std::uint64_t cache_max) noexcept {
    bool ok = true;
    try {
        MakeRequest q;
        q.r = r;
        std::memcpy(q.id, id, 32);
        q.partner_cache = partner_cache;
        q.cache_max = cache_max;
        AcquireSRWLockExclusive(&g_lock);
        try {
            g_make_queue.push_back(std::move(q));
        } catch (...) {
            ok = false;
        }
        ReleaseSRWLockExclusive(&g_lock);
    } catch (...) {
        ok = false;
    }
    if (ok) Wake();
    return ok;
}

unsigned DropQueuedMakes(const std::uint8_t (*keep)[32], unsigned n) noexcept {
    unsigned dropped = 0;
    AcquireSRWLockExclusive(&g_lock);
    const auto end = std::remove_if(g_make_queue.begin(), g_make_queue.end(), [&](const MakeRequest& q) {
        for (unsigned i = 0; i < n; ++i)
            if (std::memcmp(q.id, keep[i], 32) == 0) return false;
        return true;
    });
    dropped = static_cast<unsigned>(g_make_queue.end() - end);
    g_make_queue.erase(end, g_make_queue.end());
    ReleaseSRWLockExclusive(&g_lock);
    return dropped;
}

bool TakeMade(Made* out) noexcept {
    bool ok = false;
    AcquireSRWLockExclusive(&g_lock);
    if (!g_made.empty()) {
        *out = g_made.front();
        g_made.erase(g_made.begin());
        ok = true;
    }
    ReleaseSRWLockExclusive(&g_lock);
    return ok;
}

void SetVariant(int v) noexcept {
    g_variant.store(v >= 0 && v < df::kMetalVariants ? v : 0);
    g_rescan.store(true);
    Wake();
}
int Variant() noexcept { return g_variant.load(); }

bool OwnLook(int colour, int boots_key, Look* out) noexcept {
    if (colour < 0 || colour > 16) return false;
    if (g_cfg.dir.empty() || !g_cfg.guest) {  // no folder (or the host: Sam's player): her own look
        *out = Look{};
        out->colour = colour;
        return true;
    }
    const bool boots = boots_key >= 0 && boots_key < bootslook::kKeyCount;
    bool ok = false;
    AcquireSRWLockShared(&g_lock);
    if (g_looks[colour].settled && (!boots || g_boots_looks[boots_key].settled)) {
        *out = g_looks[colour].look;
        if (boots) {  // her suit's slots (S*) then her boots' (B*): the slot order of tex_slots
            const Look& b = g_boots_looks[boots_key].look;
            out->slots.insert(out->slots.end(), b.slots.begin(), b.slots.end());
            out->blob_ids.insert(out->blob_ids.end(), b.blob_ids.begin(), b.blob_ids.end());
        }
        ok = true;
    }
    ReleaseSRWLockShared(&g_lock);
    if (ok && boots) LookId(*out);
    return ok;
}

void SetPattern(int pattern) noexcept {
    g_pattern.store(pattern);
    if (pattern) StartWorker();
}

bool PatternLook(Look* out) noexcept {
    bool ok = false;
    AcquireSRWLockShared(&g_lock);
    if (g_pattern_built && g_pattern_built == g_pattern.load()) {
        *out = g_pattern_look;
        ok = true;
    }
    ReleaseSRWLockShared(&g_lock);
    return ok;
}

void Rescan() noexcept {
    g_rescan.store(true);
    Wake();
}
void Resend() noexcept { g_resend.store(true); }
bool TakeResend() noexcept { return g_resend.exchange(false); }
unsigned BuiltCount() noexcept { return g_built.load(); }

void Describe(char* buf, std::size_t cap) noexcept {
    AcquireSRWLockShared(&g_lock);
    int k = std::snprintf(buf, cap,
                          "worker %s, folder %s, built %u, ready-made %s (variant %d, made %u, made here for the partner %u, failed %u%s), wanted colour %d "
                          "boots key %d, looks:",
                          g_state, g_cfg.dir.empty() ? "-" : "set", g_built.load(), g_cfg.defaults ? "on" : "off", g_variant.load(), g_ready_built.load(),
                          g_made_here.load(), g_made_failed.load(), g_make_mode.load() ? ", tex 12" : "", g_wanted.load(), g_boots_wanted.load());
    for (int c = 0; c <= 16 && k < static_cast<int>(cap) - 16; ++c)
        if (g_looks[c].settled) k += std::snprintf(buf + k, cap - k, " %d:%zu", c, g_looks[c].look.slots.size());
    for (int c = 0; c < bootslook::kKeyCount && k < static_cast<int>(cap) - 16; ++c)
        if (g_boots_looks[c].settled) k += std::snprintf(buf + k, cap - k, " b%d:%zu", c, g_boots_looks[c].look.slots.size());
    ReleaseSRWLockShared(&g_lock);
}

void LookId(Look& l) noexcept {
    std::vector<std::uint8_t> b;
    for (std::size_t i = 0; i < l.slots.size(); ++i) {
        b.push_back(static_cast<std::uint8_t>(l.slots[i]));
        b.insert(b.end(), l.blob_ids.begin() + 32 * static_cast<std::ptrdiff_t>(i), l.blob_ids.begin() + 32 * static_cast<std::ptrdiff_t>(i + 1));
    }
    std::memset(l.id, 0, 32);
    if (b.empty()) return;
    tx::Sha256 h{};
    tx::HashSha256(b.data(), b.size(), h);
    std::memcpy(l.id, h.data(), 32);
}

}  // namespace st::ftex::looks
