// Story tools, game side. See story_state.hpp and docs/dev-tools.md "Story tools" for the evidence behind every address
// (the static research: the decompiles of SetEpisode 0x14335F3A0, the episode getter 0x14333C820, the FactDatabase getter
// 0x141AFC710, GetMainMissionIdArray 0x1433CD980, the mission info lookup 0x14333B210, CheckDSMissionProgressSimple
// 0x1433CA510; the load calls are coop_session.cpp's, proven by M1).
#include "story_state.hpp"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include "automation.hpp"
#include "debug_actions.hpp"
#include "game_actions.hpp"
#include "game_api.hpp"
#include "log.hpp"
#include "partner_source.hpp"
#include "steam_owner.hpp"

namespace st::story {
namespace {
namespace mem = partner::mem;
namespace sr = storyrules;

constexpr std::uintptr_t kImage = 0x140000000ull;  // ds.exe VAs at its preferred base
std::uintptr_t A(std::uintptr_t va) noexcept { return mem::Base() + (va - kImage); }

// ---------------------------------------------------------------- game addresses (ds.exe DC, PE timestamp 1706443442)
// The episode: [[[DSMissionSystemResource global] +0x28 DSMissionSystemResourceData] +0x1F0 DSMissionGoalsResource]
constexpr std::uintptr_t kGoalsHolder = 0x147C0B000;   // DSMissionSystemResource* (null outside a world)
constexpr std::uintptr_t kFactDb = 0x145794720;        // FactDatabase*
constexpr std::uintptr_t kGoalsCtx = 0x144DD9600;      // the goals facts' context GUID (data)
constexpr std::uintptr_t kFactIntGet = 0x141AFC710;    // FactDatabase int get (db, GGUUID* ctx, IntegerFact*) -> i32 (shared lock)
constexpr std::uintptr_t kGoalsRtti = 0x144DC5010;     // DSMissionGoalsResource
constexpr std::uintptr_t kFactDbRtti = 0x144885B30;    // FactDatabase
constexpr std::uintptr_t kMainRtti = 0x144DD7650;      // DSMainMissionResource (+0x1A0 EpisodeId)
constexpr std::uintptr_t kResData = 0x28, kDataGoals = 0x1F0, kEpCount = 0x40, kEpData = 0x48, kHashFact = 0x68, kIndexFact = 0xA0;
// The missions: DSMissionSystem global, its mission database +0xA8, the main/sub resource list +0x618B0 ({i32 n, i32 cap, T**})
constexpr std::uintptr_t kMissionSystem = 0x147C0AFF8;
constexpr std::uintptr_t kInfoById = 0x14333B210;      // (MS+0xA8, u64 MissionId, u8 mask 0xFF) -> DSMissionInfo* (hash lookups, no lock)
constexpr std::uintptr_t kMsDb = 0xA8, kMsList = 0x618B0;
constexpr std::uintptr_t kResId = 0x38, kResOrder = 0x40, kResCat = 0x44, kResKind = 0x46, kMainEpisode = 0x1A0;
constexpr std::uintptr_t kInfoOrderState = 0x2A;  // u8 EDSMissionOrderState (4 = completed: CountCompletedMissions)
constexpr std::uintptr_t kInfoState = 0x2C;       // u16 EDSMissionState (the current attempt: 4 = in progress)
// The load (coop_session.cpp's, M1): slot cache refresh, slot exists, the pause menu's Load Game; the globals
constexpr std::uintptr_t kContinueLast = 0x142642A00;
constexpr std::uintptr_t kSlotRefresh = 0x141A00DA0;
constexpr std::uintptr_t kHasSlot = 0x1426376E0;
constexpr std::uintptr_t kPauseLoad = 0x142636990;
constexpr std::uintptr_t kSessionGlobal = 0x147BE6318;
constexpr std::uintptr_t kAppGlobal = 0x147BE6330;

constexpr std::uint8_t kGoalsCtxBytes[16] = {0x8E, 0x80, 0x22, 0xC7, 0x37, 0xB2, 0x49, 0x42, 0x96, 0x7F, 0x85, 0xFA, 0x52, 0x90, 0x97, 0xD1};

enum Feature : unsigned { kFProbe = 1u, kFLoad = 2u };
struct Site {
    std::uintptr_t va;
    const char* sig;
    const char* name;
    unsigned features;
};
// clang-format off
constexpr Site kSites[] = {
    {0x14335F3A0, "48 89 74 24 10 57 48 83 EC 20 48 8B 05 4F BC 8A 04 33 F6 4C 8B D1 44 8B C6 48 8B 50 28 48 8B BA F0 01 00 00 48 63 47 40 85 C0 0F 8E B3 00 00 00 48 8B 57 48 4C 8B D8 48 89 5C 24 30 48 8B 19 8B",
     "SetEpisode body (goals +0x28 / +0x1F0, Episodes +0x40 / +0x48)", kFProbe},
    {0x14335F413, "45 85 C0 78 66 4C 8B 47 68 48 8D 15 DD A1 A7 01 48 8B 0D F6 52 43 02 F2 48 0F 38 F1 F3 8B",
     "SetEpisode: hash fact +0x68, context, FactDatabase, crc32", kFProbe},
    {0x14333C820, "40 57 48 83 EC 20 48 8B F9 B2 0C 48 8B 0D C6 E7 8C 04 E8 09 05 07 00 84 C0 74 08 33 C0 48 83 C4 20 5F C3 4C 8B 87 A0 00 00 00 48 8D 15 AF CD A9 01 48 8B 0D C8 7E 45 02",
     "current episode getter (index fact +0xA0)", kFProbe},
    {kFactIntGet, "48 89 5C 24 20 55 56 57 48 83 EC 20 48 8D 59 18 48 8B F9 48 8B CB 49 8B F0 48 8B EA FF 15 DE DD 03 02 84 C0 75 09 48 8B CB FF 15 E1 DD 03 02 48",
     "FactDatabase int getter", kFProbe},
    {0x1433CD980, "4C 8B DC 53 57 48 83 EC 68 48 8B 05 A0 36 B9 01 48 33 C4 48 89 44 24 48 48 8B 05 59 D6 83 04 48 8B D9 49 89 73 18 4D 89 63 E8 48 8B 90 B0 18 06 00 48 8B 72 08 48 63 02 4C 8D 24 C6 49 3B F4 0F",
     "GetMainMissionIdArray (DSMissionSystem +0x618B0, res +0x46 / +0x38)", kFProbe},
    {kInfoById, "48 89 5C 24 18 57 48 83 EC 40 48 8B 05 0F 5E C2 01 48 33 C4 48 89 44 24 38 48 89 54 24 30 41 0F",
     "mission info by id", kFProbe},
    {0x1433CA510, "48 83 EC 28 48 8B D1 48 8B 0D DA 0A 84 04 48 81 C1 A8 00 00 00 E8 66 13 F7 FF 48 85 C0 75 05 48 83 C4 28 C3 66 83 78 2C 04 0F 94 C0",
     "CheckDSMissionProgressSimple (MS +0xA8, state +0x2C)", kFProbe},
    {0x14335E670, "48 8B 01 48 85 C0 75 06 B8 02 00 00 00 C3 0F B7 40 44 C3", "GetMissionCategory (res +0x44)", kFProbe},
    {0x1433CBDB5, "48 8B 0D 3C F2 83 04 48 8B D3 48 81 C1 A8 00 00 00 E8 C5 FA F6 FF 48 85 C0 74 08 80 78 2A 04",
     "CountCompletedMissions (order state +0x2A == 4)", kFProbe},
    {kContinueLast, "48 83 3D 10 39 5A 05 00 75 11 41 B0 FF 48 8D 0D 1C 39 5A 05 B2 0F E9 55 20 FE FF C3", "ContinueLastSave (session and app globals)", kFLoad},
    {kSlotRefresh, "4C 8B DC 55 49 8D AB 28 FE FF FF 48 81 EC D0 02 00 00", "slot cache refresh", kFLoad},
    {kHasSlot, "48 83 EC 68 48 8B 05 45 99 92 02 48 33 C4", "slot exists", kFLoad},
    {kPauseLoad, "48 89 5C 24 10 48 89 6C 24 18 56 48 83 EC 40 41 0F B6 F0 0F B6 EA", "pause-menu load", kFLoad},
};
// clang-format on

// ---------------------------------------------------------------- state (game thread unless noted)
bool g_checked = false;
unsigned g_features = 0;
char g_off[160] = {};
bool g_enabled = false;
devrules::Session g_session;
double g_next_probe = 0, g_next_slots = 0;
bool g_slots_dirty = true;
sr::Summary g_last;          // the newest read
sr::Summary g_logged;        // the last one logged
bool g_logged_any = false;
int g_after_load = -1;       // the next read is logged as "after the load of manualsave<n>"

struct PendingLoad {
    bool on = false;
    int slot = -1;
    double t0 = 0;
    std::uintptr_t player = 0;  // the local player before the load
    bool gone = false;          // the old world went away (loading, no player, another player entity)
    double back_since = 0;      // in the new world since (0 = not yet)
} g_load;
char g_load_state[160] = {};

double g_now = 0;
Slot g_slots[View::kMaxSlots];
int g_slot_count = 0;

SRWLOCK g_view_lock = SRWLOCK_INIT;
View g_view;

void Put(char* out, std::size_t cap, const char* fmt, ...) {
    if (!out || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(out, cap, fmt, ap);
    va_end(ap);
}

bool RttiNamed(std::uintptr_t rtti, const char* name) {
    const char* n = mem::RttiName(rtti);
    return n && std::strcmp(n, name) == 0;
}

void Check() {
    if (g_checked) return;
    g_checked = true;
    unsigned features = kFProbe | kFLoad;
    int ok = 0, total = 0;
    auto fail = [&](unsigned f, const char* why) {
        if (!g_off[0]) Put(g_off, sizeof(g_off), "%s: %s", (f & kFProbe) ? "story probe" : "story load", why);
        features &= ~f;
    };
    for (const Site& s : kSites) {
        ++total;
        if (mem::CodeMatches(A(s.va), s.sig)) {
            ++ok;
            continue;
        }
        Log("[story] byte check FAILED: %s at 0x%llX", s.name, static_cast<unsigned long long>(s.va));
        fail(s.features, s.name);
    }
    auto rip = [&](std::uintptr_t insn, unsigned rel_off, unsigned len, std::uintptr_t want, unsigned f, const char* what) {
        ++total;
        if (mem::RipTarget(A(insn), rel_off, len) == A(want)) ++ok;
        else {
            Log("[story] byte check FAILED: %s", what);
            fail(f, what);
        }
    };
    rip(0x14335F3AA, 3, 7, kGoalsHolder, kFProbe, "SetEpisode names the DSMissionSystemResource global");
    rip(0x14335F41C, 3, 7, kGoalsCtx, kFProbe, "SetEpisode names the goals fact context");
    rip(0x14335F423, 3, 7, kFactDb, kFProbe, "SetEpisode names the FactDatabase global");
    rip(0x14333C84A, 3, 7, kGoalsCtx, kFProbe, "the episode getter names the goals fact context");
    rip(0x14333C851, 3, 7, kFactDb, kFProbe, "the episode getter names the FactDatabase global");
    rip(0x1433CD998, 3, 7, kMissionSystem, kFProbe, "GetMainMissionIdArray names the DSMissionSystem global");
    rip(0x1433CA517, 3, 7, kMissionSystem, kFProbe, "CheckDSMissionProgressSimple names the DSMissionSystem global");
    rip(0x1433CBDB5, 3, 7, kMissionSystem, kFProbe, "CountCompletedMissions names the DSMissionSystem global");
    rip(kContinueLast, 3, 8, kSessionGlobal, kFLoad, "ContinueLastSave names the session global");
    rip(kContinueLast + 0xD, 3, 7, kAppGlobal, kFLoad, "ContinueLastSave names the app global");
    ++total;
    std::uint8_t ctx[16] = {};
    if (mem::Read(A(kGoalsCtx), ctx, sizeof(ctx)) && std::memcmp(ctx, kGoalsCtxBytes, sizeof(ctx)) == 0) ++ok;
    else fail(kFProbe, "the goals fact context GUID");
    const struct {
        std::uintptr_t rtti;
        const char* name;
    } rttis[] = {{kGoalsRtti, "DSMissionGoalsResource"}, {kFactDbRtti, "FactDatabase"}, {kMainRtti, "DSMainMissionResource"}};
    for (const auto& r : rttis) {
        ++total;
        if (RttiNamed(A(r.rtti), r.name)) ++ok;
        else {
            Log("[story] byte check FAILED: RTTI %s at 0x%llX", r.name, static_cast<unsigned long long>(r.rtti));
            fail(kFProbe, r.name);
        }
    }
    g_features = features;
    Log("[story] story tools: %d/%d byte checks ok; probe %s, load %s%s%s", ok, total, (features & kFProbe) ? "on" : "OFF", (features & kFLoad) ? "on" : "OFF",
        g_off[0] ? "; " : "", g_off);
}

// ---------------------------------------------------------------- guarded game calls (no C++ objects inside)
bool CallFactInt(std::uintptr_t db, std::uintptr_t fact, std::int32_t* out) noexcept {
    __try {
        *out = reinterpret_cast<std::int32_t (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t)>(A(kFactIntGet))(db, A(kGoalsCtx), fact);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool CallInfo(std::uintptr_t msdb, std::uint64_t id, std::uintptr_t* out) noexcept {
    __try {
        *out = reinterpret_cast<std::uintptr_t (*)(std::uintptr_t, std::uint64_t, std::uint8_t)>(A(kInfoById))(msdb, id, 0xFF);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
int CallRefreshAndHas(int slot) noexcept {  // 1 exists, 0 missing, -1 fault
    __try {
        reinterpret_cast<void (*)(std::uintptr_t)>(A(kSlotRefresh))(0);
        return reinterpret_cast<std::uint8_t (*)(std::uintptr_t, std::uint8_t, std::uint8_t)>(A(kHasSlot))(A(kAppGlobal), 1, static_cast<std::uint8_t>(slot)) ? 1
                                                                                                                                                             : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
bool CallPauseLoad(int slot) noexcept {
    __try {
        reinterpret_cast<void (*)(std::uintptr_t, std::uint8_t, std::uint8_t)>(A(kPauseLoad))(A(kAppGlobal), 1, static_cast<std::uint8_t>(slot));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---------------------------------------------------------------- the world gate
// A world with a local player and nothing loading (the mission system and the facts are rebuilt on every load).
const char* NotInWorld() {
    if (!mem::Q(A(kSessionGlobal))) return "at the title";
    game::Entity* e = nullptr;
    if (!game::TryGetLocalPlayer(&e) || !e) return "no local player (loading)";
    std::uint64_t state = 0;
    std::uint32_t load = 0;
    if (!game::ReadStopGates(&state, &load)) return "the game state is unreadable";
    if (load) return "the game is loading";
    return nullptr;
}
// The world after a load: in the world and the mission goals resource there again (the mission system is rebuilt on a load).
std::uintptr_t LocalPlayer() {
    game::Entity* e = nullptr;
    return game::TryGetLocalPlayer(&e) ? reinterpret_cast<std::uintptr_t>(e) : 0;
}
bool WorldBack() {
    if (NotInWorld()) return false;
    const std::uintptr_t rs = mem::Q(A(kGoalsHolder));
    const std::uintptr_t data = rs ? mem::Q(rs + kResData) : 0;
    return data && mem::Q(data + kDataGoals);
}

// ---------------------------------------------------------------- the probe (read only)
// `why` says what was missing when false. With log_all every main / sub mission is logged.
bool ReadSummary(sr::Summary* out, bool log_all, char* why, std::size_t cap) {
    *out = sr::Summary{};
    const std::uintptr_t rs = mem::Q(A(kGoalsHolder));
    const std::uintptr_t data = rs ? mem::Q(rs + kResData) : 0;
    const std::uintptr_t goals = data ? mem::Q(data + kDataGoals) : 0;
    if (!goals || mem::RttiOf(goals) != A(kGoalsRtti)) return Put(why, cap, "the mission goals resource is not there"), false;
    const std::uintptr_t db = mem::Q(A(kFactDb));
    if (!db || mem::RttiOf(db) != A(kFactDbRtti)) return Put(why, cap, "the FactDatabase is not there"), false;
    const std::uintptr_t hash_fact = mem::Q(goals + kHashFact), index_fact = mem::Q(goals + kIndexFact);
    if (!hash_fact || !index_fact) return Put(why, cap, "the episode facts are not there"), false;
    std::int32_t index = -3, hash = 0;
    if (!CallFactInt(db, index_fact, &index) || !CallFactInt(db, hash_fact, &hash)) {
        g_features &= ~kFProbe;
        Put(g_off, sizeof(g_off), "story probe: the FactDatabase getter faulted");
        return Put(why, cap, "the FactDatabase getter faulted; the probe is off for this run"), false;
    }
    out->index_fact = index;
    out->hash_fact = static_cast<std::uint32_t>(hash);
    std::int32_t ep_n = 0;
    const std::uintptr_t ep_data = mem::Q(goals + kEpData);
    std::uint32_t hashes[sr::kEpisodeCount] = {};
    int nh = 0;
    if (mem::Rd(goals + kEpCount, ep_n) && ep_n > 0 && ep_n <= 64 && ep_data) {
        for (int i = 0; i < ep_n && i < sr::kEpisodeCount; ++i) {
            const std::uintptr_t ep = mem::Q(ep_data + 8ull * i);
            std::uint64_t g[2] = {};
            if (ep && mem::Read(ep + 8, g, sizeof(g))) hashes[i] = sr::EpisodeHash(g[0], g[1]);
            nh = i + 1;
        }
    }
    out->episode = sr::ResolveEpisode(index, static_cast<std::uint32_t>(hash), hashes, nh);

    const std::uintptr_t ms = mem::Q(A(kMissionSystem));
    const std::uintptr_t list = ms ? mem::Q(ms + kMsList) : 0;
    std::int32_t n = 0;
    const std::uintptr_t items = list ? mem::Q(list + 8) : 0;
    if (!list || !mem::Rd(list, n) || n < 0 || n > 1024 || (n && !items)) return Put(why, cap, "the mission list is not there"), false;
    static std::uintptr_t res_list[1024];
    if (n && !mem::Read(items, res_list, 8ull * static_cast<std::size_t>(n))) return Put(why, cap, "the mission list is unreadable"), false;
    std::uint32_t crc = 0;
    for (std::int32_t i = 0; i < n; ++i) {
        const std::uintptr_t res = res_list[i];
        std::uint8_t kind = 0xFF;
        if (!res || !mem::Rd(res + kResKind, kind) || kind > 1) continue;
        std::uint64_t id = 0;
        std::int32_t order = 0;
        std::uint16_t cat = 0xFFFF;
        if (!mem::Rd(res + kResId, id) || !mem::Rd(res + kResOrder, order) || !mem::Rd(res + kResCat, cat)) continue;
        std::uintptr_t info = 0;
        if (!CallInfo(ms + kMsDb, id, &info)) {
            g_features &= ~kFProbe;
            Put(g_off, sizeof(g_off), "story probe: the mission info lookup faulted");
            return Put(why, cap, "the mission info lookup faulted; the probe is off for this run"), false;
        }
        std::uint16_t state = 0;
        std::uint8_t ostate = 0;
        if (info) {
            mem::Rd(info + kInfoState, state);
            mem::Rd(info + kInfoOrderState, ostate);
        }
        const bool main = cat == 0;
        crc = sr::FoldState(crc, id, ostate, state);
        if (main) ++out->main_total;
        else ++out->sub_total;
        if (ostate == sr::kOrderCompleted) {
            if (main) {
                ++out->main_done;
                if (order > out->highest_done_order) out->highest_done_order = order;
            } else {
                ++out->sub_done;
            }
        } else if (ostate == sr::kOrderNotCompleted && main) {
            ++out->main_started;
        }
        if (state == sr::kStateProgress) sr::AddProgress(*out, id, order, main);
        else if (state == 1 || state == 2) ++out->available;
        if (log_all) {
            std::int32_t ep = -1;
            if (main && mem::RttiOf(res) == A(kMainRtti)) mem::Rd(res + kMainEpisode, ep);
            char eps[24] = "";
            if (main) Put(eps, sizeof(eps), ", episode %d", ep);
            Log("[story] mission id %llu Order %d (%s%s): order %s (%u), state %s (%u)%s", static_cast<unsigned long long>(id), order, main ? "main" : "sub", eps,
                sr::OrderStateName(ostate), ostate, sr::MissionStateName(state), state, info ? "" : " [no info]");
        }
    }
    out->crc = crc;
    out->valid = true;
    return true;
}

// ---------------------------------------------------------------- the story-point slots (the sandbox's manual slots)
// <scratch root>\DeathStrandingDC\<account>: the account folder the game uses (one numeric folder; with several, the one the
// game's own Steam id names). Only ever below the save redirect's own folder.
std::wstring AccountDir() {
    const wchar_t* root = automation::SavesScratchRoot();
    if (!root || !automation::SavesRedirected()) return {};
    const std::wstring base = std::wstring(root) + L"\\DeathStrandingDC";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((base + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::wstring found;
    const std::wstring self = std::to_wstring(static_cast<std::uint32_t>(steam::QuerySelfId() & 0xFFFFFFFFull));
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        bool digits = fd.cFileName[0] != 0;
        for (const wchar_t* p = fd.cFileName; *p; ++p) digits = digits && *p >= L'0' && *p <= L'9';
        if (!digits) continue;
        if (found.empty() || self == fd.cFileName) found = fd.cFileName;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found.empty() ? std::wstring() : base + L"\\" + found;
}

std::wstring SlotFile(const std::wstring& acc, int slot) { return acc + L"\\manualsave" + std::to_wstring(slot) + L"\\checkpoint.dat"; }

// The [Slot] text of one slot: the first 4 KiB, opened read-only and closed at once; skipped while the game writes it.
bool ReadSlotText(const std::wstring& acc, int slot, sr::SlotText* out) {
    const std::wstring f = SlotFile(acc, slot);
    if (GetFileAttributesW((f + L"temp").c_str()) != INVALID_FILE_ATTRIBUTES) return false;  // checkpoint.dattemp: being written
    HANDLE h = CreateFileW(f.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::uint8_t buf[4096];
    DWORD got = 0;
    const BOOL ok = ReadFile(h, buf, sizeof(buf), &got, nullptr);
    CloseHandle(h);
    return ok && sr::ParseSlotText(buf, got, out);
}

void ScanSlots(bool log) {
    g_slot_count = 0;
    const std::wstring acc = AccountDir();
    if (acc.empty()) {
        if (log) Log("[story] slots: no sandboxed save folder (this game plays on its real save)");
        return;
    }
    for (int i = 0; i <= sr::kMaxManualSlot && g_slot_count < View::kMaxSlots; ++i) {
        sr::SlotText t;
        if (!ReadSlotText(acc, i, &t)) continue;
        Slot& s = g_slots[g_slot_count++];
        s.index = i;
        Put(s.title, sizeof(s.title), "%s", t.title);
        Put(s.detail, sizeof(s.detail), "%s", t.detail);
        if (log)
            Log("[story] slot manualsave%d%s: %s | %s", i, i >= sr::kPackFirstSlot && i < sr::kPackFirstSlot + sr::kPackSlots ? " (story library)" : "", t.title,
                t.detail);
    }
    if (log) Log("[story] slots: %d manual slot(s) in the sandboxed save folder", g_slot_count);
}

// ---------------------------------------------------------------- the load
bool Load(int slot, char* msg, std::size_t cap) {
    const devrules::Verdict v = sr::DecideLoad(g_session);
    if (!v.ok()) {
        Log("[story] load refused: %s", v.why);
        return Put(msg, cap, "%s", v.why), false;
    }
    Check();
    if (!(g_features & kFLoad)) return Put(msg, cap, "story load is off: this game build differs (%s)", g_off), false;
    if (!sr::ValidLoadSlot(slot)) return Put(msg, cap, "manual slot 0..%d (the story library's: %d..%d)", sr::kMaxManualSlot, sr::kPackFirstSlot,
                                             sr::kPackFirstSlot + sr::kPackSlots - 1), false;
    if (g_load.on) return Put(msg, cap, "a load (manualsave%d) is still running", g_load.slot), false;
    // In a world, nothing loading (as M1's guest calls the same Load Game: any in-world state, menus and cutscenes too).
    if (const char* why = NotInWorld()) return Put(msg, cap, "load refused: %s (load from the world; at the title use the game's Load Game)", why), false;
    const std::wstring acc = AccountDir();
    if (acc.empty()) return Put(msg, cap, "load refused: no sandboxed save folder"), false;
    sr::SlotText t;
    if (!ReadSlotText(acc, slot, &t)) return Put(msg, cap, "manualsave%d is not in this sandbox (or not a save)", slot), false;
    const int has = CallRefreshAndHas(slot);
    if (has < 0) {
        g_features &= ~kFLoad;
        return Put(msg, cap, "the slot cache refresh faulted; story load off for this run"), false;
    }
    if (!has) return Put(msg, cap, "the game does not list manualsave%d after its slot refresh", slot), false;
    if (!CallPauseLoad(slot)) {
        g_features &= ~kFLoad;
        return Put(msg, cap, "the game's load faulted; story load off for this run"), false;
    }
    g_load = PendingLoad{};
    g_load.on = true;
    g_load.slot = slot;
    g_load.t0 = g_now;
    g_load.player = LocalPlayer();
    Put(g_load_state, sizeof(g_load_state), "loading manualsave%d: %s", slot, t.title);
    Log("[story] load of manualsave%d requested (the game's Load Game 0x142636990, type 1): %s | %s%s", slot, t.title, t.detail,
        g_session.role == 1 && g_session.host_save ? "; the host-held save session offers it to the guest" : "");
    return Put(msg, cap, "loading manualsave%d: %s", slot, t.title), true;
}

// A requested load: the world goes away, then the new world stands for 3 s (the mission system rebuilt), or a timeout.
void LoadTick() {
    if (!g_load.on) return;
    const double t = (g_now - g_load.t0) / 1000.0;
    const char* why = NotInWorld();
    const std::uintptr_t player = LocalPlayer();
    if (!g_load.gone) {
        if (why || player != g_load.player) {
            g_load.gone = true;
            Log("[story] load of manualsave%d: the world unloads after %.1f s (%s)", g_load.slot, t, why ? why : "a new local player");
        } else if (t > 30) {
            Log("[story] load of manualsave%d: the game did not start it within 30 s: FAILED", g_load.slot);
            Put(g_load_state, sizeof(g_load_state), "manualsave%d: the game did not start the load", g_load.slot);
            g_load.on = false;
        }
        return;
    }
    if (!WorldBack()) {
        g_load.back_since = 0;
    } else if (g_load.back_since == 0) {
        g_load.back_since = g_now;
    }
    if (g_load.back_since > 0 && g_now - g_load.back_since >= 3000) {
        Log("[story] load of manualsave%d: in the world after %.1f s", g_load.slot, t);
        Put(g_load_state, sizeof(g_load_state), "manualsave%d loaded (%.0f s)", g_load.slot, t);
        g_after_load = g_load.slot;
        g_next_probe = 0;
        g_slots_dirty = true;
        g_load.on = false;
    } else if (t > 180) {
        Log("[story] load of manualsave%d: not in the world within 180 s (%s): FAILED", g_load.slot, why ? why : "no mission goals yet");
        Put(g_load_state, sizeof(g_load_state), "manualsave%d: no world within 180 s", g_load.slot);
        g_load.on = false;
    }
}

void Probe() {
    if (g_load.on) return;  // our own load: its new world is logged once it stands ("after the load of ...")
    if (!(g_features & kFProbe) || NotInWorld()) {
        g_last.valid = false;
        return;
    }
    sr::Summary s;
    char why[160] = {};
    static char last_why[160] = {};
    if (!ReadSummary(&s, false, why, sizeof(why))) {
        g_last.valid = false;
        if (std::strcmp(why, last_why) != 0) Log("[story] probe: no story state in this world yet (%s)", why);
        Put(last_why, sizeof(last_why), "%s", why);
        return;
    }
    last_why[0] = 0;
    g_last = s;
    char line[320];
    sr::FormatSummary(s, line, sizeof(line));
    if (g_after_load >= 0) {
        Log("[story] after the load of manualsave%d: %s", g_after_load, line);
        g_after_load = -1;
    } else if (!g_logged_any || !sr::SameStory(s, g_logged)) {
        if (g_logged_any && g_logged.episode != s.episode) {
            char a[24], b[24];
            sr::EpisodeLabel(g_logged.episode, a, sizeof(a));
            sr::EpisodeLabel(s.episode, b, sizeof(b));
            Log("[story] episode changed: %s -> %s", a, b);
        }
        Log("[story] %s%s", g_logged_any ? "changed: " : "", line);
    } else {
        return;
    }
    g_logged = s;
    g_logged_any = true;
}

void FillView() {
    View v;
    v.probe_ok = (g_features & kFProbe) != 0;
    v.load_ok = (g_features & kFLoad) != 0;
    Put(v.off, sizeof(v.off), "%s", g_off);
    v.summary = g_last;
    sr::FormatSummary(g_last, v.line, sizeof(v.line));
    const devrules::Verdict d = sr::DecideLoad(g_session);
    v.load_allowed = d.ok() && v.load_ok;
    Put(v.load_why, sizeof(v.load_why), "%s", !d.ok() ? d.why : !v.load_ok ? "off: this game build differs" : "");
    Put(v.load_state, sizeof(v.load_state), "%s", g_load_state);
    v.slot_count = g_slot_count;
    for (int i = 0; i < g_slot_count; ++i) v.slots[i] = g_slots[i];
    AcquireSRWLockExclusive(&g_view_lock);
    v.serial = g_view.serial + 1;
    g_view = v;
    ReleaseSRWLockExclusive(&g_view_lock);
}

}  // namespace

void Tick(double now_ms, bool enabled, const devrules::Session& session) noexcept {
    g_now = now_ms;
    g_enabled = enabled;
    g_session = session;
    if (!enabled) return;
    LoadTick();
    if (now_ms < g_next_probe) return;
    g_next_probe = now_ms + 1000;
    Check();
    Probe();
    // the slot list: on demand, after a load, and every 5 s while the menu is open (sandboxed saves only)
    if (session.saves_redirected && (g_slots_dirty || (debug::MenuOpen() && now_ms >= g_next_slots))) {
        ScanSlots(g_slots_dirty);
        g_slots_dirty = false;
        g_next_slots = now_ms + 5000;
    }
    FillView();
}

bool Command(Op op, const double (&v)[3], char* msg, std::size_t cap) noexcept {
    if (!g_enabled) return Put(msg, cap, "%s", devrules::kOffReason), false;
    Check();
    if (op == Op::load) return Load(static_cast<int>(v[0]), msg, cap);
    const int what = static_cast<int>(v[0]);
    if (what == 3) {  // the load's gate in this session (scenarios check a refusal with it: a refused story_load fails its step)
        const devrules::Verdict d = sr::DecideLoad(g_session);
        Log("[story] load gate: %s", d.ok() ? "allowed" : d.why);
        return Put(msg, cap, "story_load %s", d.ok() ? "allowed here" : d.why), true;
    }
    if (what == 2) {
        ScanSlots(true);
        FillView();
        return Put(msg, cap, "%d manual slot(s) logged", g_slot_count), g_slot_count > 0;
    }
    if (!(g_features & kFProbe)) return Put(msg, cap, "the story probe is off: this game build differs (%s)", g_off), false;
    if (const char* why = NotInWorld()) return Put(msg, cap, "no story state: %s", why), false;
    sr::Summary s;
    char why[160] = {};
    if (!ReadSummary(&s, what == 1, why, sizeof(why))) return Put(msg, cap, "no story state: %s", why), false;
    g_last = s;
    char line[320];
    sr::FormatSummary(s, line, sizeof(line));
    Log("[story] summary: %s; facts: episode index %d, episode hash 0x%08X", line, s.index_fact, s.hash_fact);
    FillView();
    return Put(msg, cap, "%s", line), true;
}

storyrules::Summary GetSummary() noexcept {
    AcquireSRWLockShared(&g_view_lock);
    const storyrules::Summary s = g_view.summary;
    ReleaseSRWLockShared(&g_view_lock);
    return s;
}

View GetView() noexcept {
    AcquireSRWLockShared(&g_view_lock);
    View v = g_view;
    ReleaseSRWLockShared(&g_view_lock);
    return v;
}

}  // namespace st::story
