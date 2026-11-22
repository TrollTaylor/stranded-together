#include "texture_looks.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "boots_look.hpp"
#include "log.hpp"
#include "png_wic.hpp"
#include "tex_blob.hpp"
#include "tex_hash.hpp"
#include "tex_manifest.hpp"
#include "tex_slots.hpp"

namespace st::ftex::looks {
namespace {

namespace tx = st::tex;

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

// worker-only
struct HashEntry {
    FileStamp st;
    std::string sha;
};
std::map<std::wstring, HashEntry> g_hashes;
tx::Manifest g_manifest;
bool g_manifest_ok = false;
FileStamp g_manifest_stamp;

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
        if (g_manifest_ok || g_manifest_stamp.size != 1) Log("[tex] no %s in the textures folder: press Extract textures in the launcher", tx::kManifestName);
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

void PruneBuiltCache() {
    const std::wstring dir = CacheDir();
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
    if (total <= kBuiltCacheMax) return;
    std::sort(files.begin(), files.end(), [](const F& a, const F& b) { return a.mtime < b.mtime; });
    for (const F& f : files) {
        if (total <= kBuiltCacheMax) break;
        if (DeleteFileW(f.p.c_str())) total -= f.size;
    }
}

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
        // the size from the header first: a picture far bigger than the slot is refused before it is decoded (a
        // 16384 x 16384 PNG would need 1 GiB in the game's process)
        std::vector<std::uint8_t> png;
        std::uint32_t pw = 0, ph = 0;
        if (!ReadAll(comps[i].first, png) || !tx::PngSize(png.data(), png.size(), pw, ph)) {
            Log("[tex] %s: %s does not read as a PNG: slot skipped", s.id, comps[i].second.c_str());
            return false;
        }
        if (pw > tx::MaxEditSide(s.w) || ph > tx::MaxEditSide(s.h)) {
            Log("[tex] %s: %s is %ux%u, too big (at most %ux%u; save it at %ux%u): slot skipped", s.id, comps[i].second.c_str(), pw, ph, tx::MaxEditSide(s.w),
                tx::MaxEditSide(s.h), s.w, s.h);
            return false;
        }
        if (!tx::DecodePng(png.data(), png.size(), img, &err)) {
            Log("[tex] %s: %s does not read as a PNG (%s): slot skipped", s.id, comps[i].second.c_str(), err.c_str());
            return false;
        }
        std::vector<std::uint8_t>().swap(png);
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

// One colour's look from the files. False when the folder cannot be read (the look stays unsettled).
bool BuildColour(int colour, Look& out, std::string& sig_out) {
    out = Look{};
    out.colour = colour;
    const std::string folder = ColourFolder(colour);
    if (folder.empty()) {
        sig_out = "none";
        return true;  // no folder: her own look
    }
    std::string sig, names;
    char what[32];
    std::snprintf(what, sizeof(what), "colour %d", colour);
    BuildFolder(tx::SlotGroup::suit, folder, kSuitDir, colour, what, out, sig, names);
    LookId(out);
    sig_out = sig.empty() ? "none" : sig;
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
                if ((need || need_boots) && c >= 0 && c <= 16 && LoadManifest()) {
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
        }
        Sleep(250);
    }
    // never reached (the worker lives for the process); kept for symmetry
    // if (watch != INVALID_HANDLE_VALUE) FindCloseChangeNotification(watch);
    // if (SUCCEEDED(co)) CoUninitialize();
    (void)co;
}

void StartWorker() {
    if (g_started) return;
    g_started = true;
    std::thread(Worker).detach();
}

}  // namespace

void Configure(const Settings& s) noexcept {
    g_cfg = s;
    if (s.on && ((!s.dir.empty() && s.guest) || g_pattern.load())) StartWorker();
}

void SetWantedColour(int colour) noexcept { g_wanted.store(colour); }
void SetWantedBoots(int key) noexcept { g_boots_wanted.store(key >= 0 && key < bootslook::kKeyCount ? key : -1); }

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

void Rescan() noexcept { g_rescan.store(true); }
void Resend() noexcept { g_resend.store(true); }
bool TakeResend() noexcept { return g_resend.exchange(false); }
unsigned BuiltCount() noexcept { return g_built.load(); }

void Describe(char* buf, std::size_t cap) noexcept {
    AcquireSRWLockShared(&g_lock);
    int k = std::snprintf(buf, cap, "worker %s, folder %s, built %u, wanted colour %d boots key %d, looks:", g_state, g_cfg.dir.empty() ? "-" : "set",
                          g_built.load(), g_wanted.load(), g_boots_wanted.load());
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
