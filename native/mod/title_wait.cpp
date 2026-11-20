// The guest's title screen in the game's own menu. See title_wait.hpp and docs/coop-session.md "The guest's title screen".
#include "title_wait.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "MinHook.h"
#include "coop_rules.hpp"
#include "link.hpp"
#include "log.hpp"
#include "partner_source.hpp"

namespace st::title_wait {
namespace {
namespace mem = partner::mem;

constexpr std::uintptr_t kImage = 0x140000000ull;  // ds.exe VAs at its preferred base
std::uintptr_t A(std::uintptr_t va) noexcept { return mem::Base() + (va - kImage); }

// ---------------------------------------------------------------- game addresses (ds.exe, Director's Cut)
constexpr std::uintptr_t kDsUpdate = 0x143273060;    // DSUITitleMenuDataSource::Update(this) (vt[5])
constexpr std::uintptr_t kTmfUpdate = 0x143278AB0;   // DSUITitleMenuFunction::Update(this, float dt) (vt[14])
constexpr std::uintptr_t kTmfInit = 0x143278430;     // DSUITitleMenuFunction::Init(this) (vt[13]): the field map below
constexpr std::uintptr_t kTmfInitMenuSys = 0x143278469;  // in Init: mov rcx, [menu system] (names the global)
constexpr std::uintptr_t kMapFind = 0x141B257D0;     // data-source map find-or-create (ds+0x20, ds+0x28, const u32* key, 0) -> entry
constexpr std::uintptr_t kStrAssignC = 0x14191CBC0;  // Decima String assign from a C string (String*, const char*)
constexpr std::uintptr_t kSetVisible = 0x141B56850;  // Tag SetVisible (tag, u8): tag+0xB0, the layout marked dirty
constexpr std::uintptr_t kFindTag = 0x141B1DE10;     // FindTagByResource (menu system, element resource, 0) -> tag
constexpr std::uintptr_t kOnContinue = 0x1432778F0;  // DSUITitleMenuFunction::OnContinueGame(this)
constexpr std::uintptr_t kOnNewGame = 0x1432799F0;   // DSUITitleMenuFunction::OnNewGame(this): the "start a new game?" dialog
constexpr std::uintptr_t kOnLoadSlot = 0x14327B010;  // DSUITitleMenuFunction::OnSelectLoadSlot(this): the load page
constexpr std::uintptr_t kOnExit = 0x143277AD0;      // DSUITitleMenuFunction::OnExitGame: left alone (checked: EXIT GAME quits)
constexpr std::uintptr_t kDsVtable = 0x143E94C98;    // DSUITitleMenuDataSource
constexpr std::uintptr_t kTmfVtable = 0x143E94BE8;   // DSUITitleMenuFunction
constexpr std::uintptr_t kMenuSysGlobal = 0x1457965F8;
constexpr std::uintptr_t kUiGlobalsRead = 0x1430125B9;  // in GetUIGlobals: mov rax, [UI globals] (names the global)
constexpr std::uintptr_t kUiGlobals = 0x147C00128;      // *(u64*): UI globals; +0x2D0 = the title menu's state
constexpr std::uintptr_t kUiTitleState = 0x2D0, kTitleLabelsDirty = 0x81;   // state+0x81: DS::Update rewrites the 8 labels

// The boot logos (DSUISplashScreenMenuFunction, the page before the title page; docs/coop-session.md "The guest's title
// screen"): its Update's state 0 holds the game's own "skip the whole splash" branch (0x1432787CC: a platform check that
// is always false on PC, then SetFocus(this+0xA8) and "page finished" to the menu system, which opens the title page).
// The guest takes that branch on the splash's first update; everything else is the game's own code.
constexpr std::uintptr_t kSplashUpdate = 0x143278770;     // DSUISplashScreenMenuFunction::Update(this, float dt) (vt[14])
constexpr std::uintptr_t kSplashSkipAll = 0x1432787CC;    // its skip-all branch (state 0)
constexpr std::uintptr_t kSplashSkipMenuSys = 0x1432787F2;  // in it: mov rcx, [menu system]
constexpr std::uintptr_t kSplashPlatform = 0x14577D3F0;   // the platform object its check asks (named by the branch)
constexpr std::uintptr_t kSplashDialogRead = 0x14327887B; // state 1: cmp byte [settings-reset dialog flag], 0
constexpr std::uintptr_t kSplashDialogFlag = 0x145558F78; // set when the graphics init reset the display mode: a dialog
constexpr std::uintptr_t kSplashOnDecima = 0x143277BC0;   // OnFinishDecimaLogo: the same end call (reference)
constexpr std::uintptr_t kPageFinished = 0x141B1EB60;     // (menu system): the current page finished -> the next page
constexpr std::uintptr_t kSetFocus = 0x141B4E7D0;         // (tag, tag, u8)
constexpr std::uintptr_t kSplashVtable = 0x143E94988;
constexpr std::uintptr_t kSplashState = 0xA0, kSplashNext = 0xA1, kSplashEndTag = 0xA8;

// Data-source keys (crc32c of the binding's name) and layout (DS::Update's own writes, docs/coop-session.md).
constexpr std::uint32_t kKeyHasSave = 0x7339FA72;    // bool: CONTINUE and LOAD GAME visible (HasSlot(app, 0xF, 0xFF))
constexpr std::uint32_t kKeyPs4 = 0x1A8722BC;        // bool: the PS4->PS5 item visible (DS::Refresh sets it)
constexpr std::uint32_t kKeyNewGame = 0x06F2B2AF;    // string: NEW GAME's label (txt_new_game, txt_new_game_bg)
constexpr std::uintptr_t kDsMapHdr = 0x20, kDsMap = 0x28, kDsDirty = 0x60, kDsReevalBools = 0x88;
constexpr std::uintptr_t kEntBool = 0x20, kEntStr = 0x28, kEntValid = 0x30;
// DSUITitleMenuFunction fields (Init): +0x50 its resource; +0xE0 lst_title_menu_list's tag. The resource's +0x70 =
// lst_title_menu_list's MenuListResource; its ChildElements array {u32 count, u32 cap, Ref* data} at +0x180; every
// element resource's name (a Decima String: char*) at +0x20. Tags: +0xB0 visible; a list's children at +0x20 (count)
// and +0x28 (Tag**).
constexpr std::uintptr_t kTmfRes = 0x50, kTmfList = 0xE0, kResList = 0x70, kListChildren = 0x180, kResName = 0x20;
constexpr std::uintptr_t kTagVisible = 0xB0, kTagCount = 0x20, kTagChildren = 0x28;

struct Site {
    std::uintptr_t va;
    const char* sig;
    const char* name;
};
// clang-format off
constexpr Site kSites[] = {
    {kDsUpdate,       "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 41 54 41 55 41 56 41 57 48 8D AC 24 A0 FC FF FF", "title data source Update"},
    {kTmfUpdate,      "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 40 C5 F8 29 74 24 30", "title menu Update"},
    {kTmfInit,        "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 48 8B D9 E8 54 A1 D9 FF", "title menu Init"},
    {kTmfInitMenuSys, "48 8B 0D 88 E1 51 02 48 8B 55 30 E8", "title menu Init (menu system)"},
    {kMapFind,        "48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 49 8B F0 48 8B FA 48 8B E9", "data source find-or-create"},
    {kStrAssignC,     "40 53 48 83 EC 20 48 8B D9 49 C7 C0 FF FF FF FF 49 FF C0 42 80 3C 02 00", "string assign"},
    {kSetVisible,     "3A 91 B0 00 00 00 74 1F 48 8B 05 99 FD C3 03 4C 8B 80 B8 00 00 00", "tag SetVisible"},
    {kFindTag,        "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B E9 49", "find tag by resource"},
    {kOnContinue,     "40 53 48 83 EC 20 48 8B 91 08 01 00 00 48 8B D9 E8 6B CF 00 00", "OnContinueGame"},
    {kOnNewGame,      "40 53 48 83 EC 20 48 8B D9 E8 A2 8B D9 FF BA 03 00 00 00 B9 01 00 00 00", "OnNewGame"},
    {kOnLoadSlot,     "48 83 EC 28 48 8B 91 18 01 00 00 E8 50 98 00 00 E8 3B 05 FD FF C6 40 30", "OnSelectLoadSlot"},
    {kOnExit,         "48 83 EC 28 66 C7 05 7B E8 96 04 01 01 E8 BE AA D9 FF", "OnExitGame"},
    {kUiGlobalsRead,  "48 8B 05 68 DB BE 04 48 85 C0 75 1B", "UI globals (title menu state)"},
};
// The boot-logo skip's own checks: a mismatch leaves only the skip off (the logos play).
constexpr Site kSkipSites[] = {
    {kSplashUpdate,     "48 89 5C 24 10 57 48 83 EC 30 C5 F8 29 74 24 20 48 8B D9 C5 F8 28 F1 E8 14 9E D9 FF C5 CA 58 83", "splash Update"},
    {kSplashSkipAll,    "48 8B 0D 1D 4C 50 02 48 8B 01 FF 90 58 01 00 00 84 C0 74 28 48 8B 8B A8 00 00 00 45 33 C0 48 8B D1 E8 DE 5F 8D FE "
                        "48 8B 0D FF DD 51 02 48 8B 5C 24 48 48 83 C4 30 5F E9 58 63 8A FE", "splash skip-all branch"},
    {kSplashDialogRead, "80 3D F6 06 2E 02 00", "splash settings-dialog flag"},
    {kSplashOnDecima,   "C7 81 F8 00 00 00 00 00 00 00 48 8B 0D 27 EA 51 02 E9 8A 6F 8A FE", "splash OnFinishDecimaLogo"},
    {kPageFinished,     "48 89 5C 24 08 57 48 83 EC 30 48 8B F9 48 8D 0D 4C F8 D6 02 E8 77 27 ED", "menu page finished"},
    {kSetFocus,         "48 8B 0D 21 7E C4 03 45 0F B6 C8 45 33 C0 48 8B 89 B8 00 00 00 E9 16 27", "menu SetFocus"},
};
// clang-format on

// ---------------------------------------------------------------- function types and state
using FnDsUpdate = void (*)(std::uintptr_t);
using FnTmfUpdate = void (*)(std::uintptr_t, float);
using FnHandler = void (*)(std::uintptr_t);
using FnMapFind = std::uintptr_t (*)(std::uintptr_t, std::uintptr_t, const std::uint32_t*, int);
using FnStrAssign = std::uintptr_t (*)(std::uintptr_t, const char*);
using FnSetVisible = void (*)(std::uintptr_t, std::uint8_t);
using FnFindTag = std::uintptr_t (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t);

using FnSplashUpdate = void (*)(std::uintptr_t, float);
using FnSetFocus = void (*)(std::uintptr_t, std::uintptr_t, std::uint8_t);
using FnPageFinished = void (*)(std::uintptr_t);

FnDsUpdate o_ds = nullptr;
FnTmfUpdate o_tmf = nullptr;
FnHandler o_continue = nullptr, o_newgame = nullptr, o_loadslot = nullptr;
FnSplashUpdate o_splash = nullptr;
bool g_installed = false;
std::atomic<bool> g_skip_armed{false};     // the boot-logo skip is in and not used yet
std::atomic<int> g_skip_result{0};         // 0 not tried, 1 skipped, -1 faulted, -2 not taken (the splash not in its first state)
std::atomic<double> g_skip_ms{0};          // when the logos were skipped (link::NowMsPrecise)
std::atomic<double> g_first_page_ms{0};    // the first title page's first update (link::NowMsPrecise)

constexpr std::size_t kLabelCap = 96;
SRWLOCK g_mx = SRWLOCK_INIT;  // g_label (game thread -> the menu's thread)
char g_label[kLabelCap] = {};
std::atomic<bool> g_want{false};
char g_logged_label[kLabelCap] = {};  // game thread: the last label logged

// Menu thread only (the two Update hooks run on the game's menu update; the handlers on its input dispatch, the same
// thread as the menu's update: they are called from the menu's event processing).
struct Saved {
    bool have = false;
    std::uint8_t b = 0, valid = 0;
    char text[160] = {};
};
struct DsState {
    std::uintptr_t ds = 0;  // the data source these belong to
    Saved has_save, ps4, label;
    bool applied = false;
};
DsState g_ds;
constexpr int kMaxItems = 16;
struct TmfState {
    std::uintptr_t self = 0, list = 0;
    bool resolved = false, resolve_failed = false;
    int n_hide = 0;
    std::uintptr_t hide[kMaxItems] = {};  // tags to hide (verified children of `list`)
    char hide_names[kMaxItems][40] = {};
    bool hidden[kMaxItems] = {};          // hidden by us (restored when the session wants the game's menu back)
    bool told = false;
    unsigned page = 0;                    // the how-manieth title page this game showed (logs)
};
TmfState g_tmf;
unsigned g_pages = 0;

std::atomic<double> g_tmf_seen_ms{-1e18};
double g_ds_seen_ms = -1e18;              // menu thread: the data source's last update
// No update of the title menu for this long: the page may have been destroyed and a new one built at the same addresses
// (the allocator reuses same-size blocks), so nothing cached by address alone is trusted (H_tmf, H_ds).
constexpr double kPageGapMs = 1000;
std::atomic<bool> g_label_on{false};      // the label is in the current data source
std::atomic<unsigned> g_ignored{0};       // presses on the label ignored
std::atomic<unsigned> g_hidden_count{0};
std::atomic<unsigned> g_faults{0};

bool Want() noexcept { return g_installed && g_want.load(); }

void SnapshotLabel(char (&out)[kLabelCap]) noexcept {
    AcquireSRWLockShared(&g_mx);
    std::memcpy(out, g_label, kLabelCap);
    ReleaseSRWLockShared(&g_mx);
    out[kLabelCap - 1] = 0;
}

// A C string at `p` read fault-free (ReadProcessMemory per byte; names are short and read once per page).
void ReadName(std::uintptr_t p, char* out, std::size_t cap) noexcept {
    std::size_t i = 0;
    for (; p && i + 1 < cap; ++i) {
        char c = 0;
        if (!mem::Rd(p + i, c) || !c) break;
        out[i] = c;
    }
    out[i] = 0;
}

// ---------------------------------------------------------------- the data source (SEH: only PODs here)
std::uintptr_t Entry(std::uintptr_t ds, std::uint32_t key) noexcept {
    const std::uint32_t k = key;
    return reinterpret_cast<FnMapFind>(A(kMapFind))(ds + kDsMapHdr, ds + kDsMap, &k, 0);
}
void Save(std::uintptr_t e, Saved& s) noexcept {
    if (s.have) return;
    s.have = true;
    s.b = *reinterpret_cast<std::uint8_t*>(e + kEntBool);
    s.valid = *reinterpret_cast<std::uint8_t*>(e + kEntValid);
    s.text[0] = 0;
    const char* p = *reinterpret_cast<const char* const*>(e + kEntStr);
    if (p && s.valid) {
        std::size_t i = 0;
        for (; p[i] && i + 1 < sizeof(s.text); ++i) s.text[i] = p[i];
        s.text[i] = 0;
    }
}
// The game's own write pattern: compare, then assign, valid = 1, the data source dirty.
void WriteBool(std::uintptr_t ds, std::uint32_t key, bool v, Saved& s) noexcept {
    const std::uintptr_t e = Entry(ds, key);
    if (!e) return;
    Save(e, s);
    std::uint8_t& b = *reinterpret_cast<std::uint8_t*>(e + kEntBool);
    std::uint8_t& valid = *reinterpret_cast<std::uint8_t*>(e + kEntValid);
    if (valid && (b != 0) == v) return;
    b = v ? 1 : 0;
    reinterpret_cast<FnStrAssign>(A(kStrAssignC))(e + kEntStr, v ? "true" : "false");
    valid = 1;
    *reinterpret_cast<std::uint8_t*>(ds + kDsDirty) = 1;
}
bool WriteText(std::uintptr_t ds, std::uint32_t key, const char* text, Saved& s) noexcept {
    const std::uintptr_t e = Entry(ds, key);
    if (!e) return false;
    Save(e, s);
    std::uint8_t& valid = *reinterpret_cast<std::uint8_t*>(e + kEntValid);
    const char* p = *reinterpret_cast<const char* const*>(e + kEntStr);
    if (valid && p && std::strcmp(p, text) == 0) return true;
    reinterpret_cast<FnStrAssign>(A(kStrAssignC))(e + kEntStr, text);
    valid = 1;
    *reinterpret_cast<std::uint8_t*>(ds + kDsDirty) = 1;
    return true;
}
void Restore(std::uintptr_t ds, std::uint32_t key, const Saved& s, bool is_bool) noexcept {
    if (!s.have) return;
    const std::uintptr_t e = Entry(ds, key);
    if (!e) return;
    if (is_bool) *reinterpret_cast<std::uint8_t*>(e + kEntBool) = s.b;
    reinterpret_cast<FnStrAssign>(A(kStrAssignC))(e + kEntStr, s.text);
    *reinterpret_cast<std::uint8_t*>(e + kEntValid) = s.valid ? 1 : 0;
    *reinterpret_cast<std::uint8_t*>(ds + kDsDirty) = 1;
}

// 1 applied, 2 restored, 0 nothing, -1 fault.
int DsPostSeh(std::uintptr_t ds, bool want, const char* label) noexcept {
    __try {
        if (want) {
            WriteBool(ds, kKeyHasSave, false, g_ds.has_save);
            WriteBool(ds, kKeyPs4, false, g_ds.ps4);
            if (!WriteText(ds, kKeyNewGame, label, g_ds.label)) return 0;
            g_ds.applied = true;
            return 1;
        }
        if (!g_ds.applied) return 0;
        Restore(ds, kKeyHasSave, g_ds.has_save, true);
        Restore(ds, kKeyPs4, g_ds.ps4, true);
        Restore(ds, kKeyNewGame, g_ds.label, false);
        *reinterpret_cast<std::uint8_t*>(ds + kDsReevalBools) = 1;   // the game's own flag: "has a save" asked again
        // The game's own flag: its next DS::Update writes all 8 labels from the localized texts again (the label may have
        // been ours from the first frame of the page, before the game wrote its own).
        const std::uintptr_t ui = mem::Q(A(kUiGlobals));
        const std::uintptr_t state = ui ? mem::Q(ui + kUiTitleState) : 0;
        if (state) *reinterpret_cast<std::uint8_t*>(state + kTitleLabelsDirty) = 1;
        g_ds = DsState{};
        g_ds.ds = ds;
        return 2;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

void H_ds(std::uintptr_t ds) {
    o_ds(ds);
    std::uintptr_t vt = 0;
    if (!g_installed || !mem::Rd(ds, vt) || vt != A(kDsVtable)) return;
    const double now = link::NowMsPrecise();
    const bool gap = now - g_ds_seen_ms > kPageGapMs;
    g_ds_seen_ms = now;
    // Another data source (a new title page): nothing of ours is in it. After a gap the same address may be a new page
    // too; the entries are looked up by key on every write, so only what we applied is kept (the restore needs it).
    if (g_ds.ds != ds || (gap && !g_ds.applied)) {
        g_ds = DsState{};
        g_ds.ds = ds;
        g_label_on.store(false);
    }
    const bool want = Want();
    if (!want && !g_ds.applied) return;
    char label[kLabelCap];
    SnapshotLabel(label);
    if (want && !label[0]) return;
    const int r = DsPostSeh(ds, want, label);
    if (r == 1) g_label_on.store(true);
    else if (r == 2) {
        g_label_on.store(false);
        Log("[title] the game's own title menu is back (labels and items restored)");
    } else if (r < 0) {
        g_label_on.store(false);
        const unsigned n = g_faults.fetch_add(1) + 1;
        if (n <= 3) Log("[title] FAULT in the title data source write #%u", n);
    }
}

// ---------------------------------------------------------------- the menu list (items hidden by name)
bool IsChild(std::uintptr_t list, std::uintptr_t tag) noexcept {
    std::int32_t count = 0;
    std::uintptr_t data = 0;
    if (!mem::Rd(list + kTagCount, count) || !mem::Rd(list + kTagChildren, data) || count <= 0 || count > 64 || !data) return false;
    for (std::int32_t i = 0; i < count; ++i)
        if (mem::Q(data + static_cast<std::uintptr_t>(i) * 8) == tag) return true;
    return false;
}

std::uintptr_t CallFindTag(std::uintptr_t menu_sys, std::uintptr_t res) noexcept {
    __try {
        return reinterpret_cast<FnFindTag>(A(kFindTag))(menu_sys, res, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
bool CallSetVisible(std::uintptr_t tag, bool v) noexcept {
    __try {
        reinterpret_cast<FnSetVisible>(A(kSetVisible))(tag, v ? 1 : 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The items to hide, by element name: lst_title_menu_list's resource children -> FindTagByResource (as the game's own
// Init does) -> kept only when the tag is a child of the list's tag. Reads are fault-free; a layout that does not
// match (another build) leaves the items alone (the label still works).
void Resolve(std::uintptr_t self, std::uintptr_t list, TmfState& s) noexcept {
    s.resolved = true;
    char why[120] = "";
    char all[400] = "";
    const std::uintptr_t res = mem::Q(self + kTmfRes);
    const std::uintptr_t lres = res ? mem::Q(res + kResList) : 0;
    const std::uintptr_t menu_sys = mem::Q(A(kMenuSysGlobal));
    char lname[48] = "";
    if (lres) ReadName(mem::Q(lres + kResName), lname, sizeof(lname));
    std::uint32_t count = 0;
    const std::uintptr_t data = lres ? mem::Q(lres + kListChildren + 8) : 0;
    if (!res || !lres || !menu_sys) std::snprintf(why, sizeof(why), "the menu's resource or menu system is not there");
    else if (std::strcmp(lname, "lst_title_menu_list") != 0) std::snprintf(why, sizeof(why), "the list resource is '%s', not lst_title_menu_list", lname);
    else if (!mem::Rd(lres + kListChildren, count) || count == 0 || count > kMaxItems || !data)
        std::snprintf(why, sizeof(why), "the list has %u children", count);
    if (why[0]) {
        s.resolve_failed = true;
        Log("[title] the menu items cannot be resolved (title page #%u: %s): only the label is changed", s.page, why);
        return;
    }
    std::size_t at = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uintptr_t item = mem::Q(data + i * 8ull);
        char name[40] = "";
        if (item) ReadName(mem::Q(item + kResName), name, sizeof(name));
        at += static_cast<std::size_t>(std::snprintf(all + at, at < sizeof(all) ? sizeof(all) - at : 0, "%s%s", i ? "," : "", name[0] ? name : "?"));
        if (at >= sizeof(all)) at = sizeof(all) - 1;
        if (!item || !coop::rules::TitleItemHidden(name)) continue;
        const std::uintptr_t tag = CallFindTag(menu_sys, item);
        if (!tag || !IsChild(list, tag)) {
            Log("[title] %s: no tag in the list (%s): left alone", name, tag ? "not a child of lst_title_menu_list" : "not found");
            continue;
        }
        bool known = false;   // kept from before a gap (Revalidate): still hidden by us
        for (int k = 0; k < s.n_hide; ++k) known = known || s.hide[k] == tag;
        if (!known && s.n_hide < kMaxItems) {
            std::snprintf(s.hide_names[s.n_hide], sizeof(s.hide_names[0]), "%s", name);
            s.hide[s.n_hide++] = tag;
        }
    }
    Log("[title] lst_title_menu_list (title page #%u): %u items (%s); %d to hide", s.page, count, all, s.n_hide);
}

// The menu had no update for a while but sits at the same addresses: perhaps a new page in the old one's memory. Only the
// tags still children of the current list (read fresh from the vtable-checked menu) are kept, with their hidden flag (so
// the game's menu still gets them back); the rest is resolved again from the page's own resources.
void Revalidate(std::uintptr_t self, std::uintptr_t list, TmfState& s, double gap_ms) noexcept {
    const TmfState old = s;
    s = TmfState{};
    s.self = self;
    s.list = list;
    s.page = old.page;
    for (int i = 0; i < old.n_hide; ++i) {
        if (!old.hidden[i] || !IsChild(list, old.hide[i])) continue;
        std::snprintf(s.hide_names[s.n_hide], sizeof(s.hide_names[0]), "%s", old.hide_names[i]);
        s.hidden[s.n_hide] = true;
        s.hide[s.n_hide++] = old.hide[i];
    }
    s.told = old.told && s.n_hide > 0;
    Log("[title] the title menu had no update for %.1f s at the same address: its items are resolved again (%d of %d kept, still in "
        "the list)",
        gap_ms / 1000.0, s.n_hide, old.n_hide);
}

void H_tmf(std::uintptr_t self, float dt) {
    o_tmf(self, dt);
    std::uintptr_t vt = 0;
    if (!g_installed || !mem::Rd(self, vt) || vt != A(kTmfVtable)) return;
    const std::uintptr_t list = mem::Q(self + kTmfList);
    if (!list) return;
    const double now = link::NowMsPrecise();
    const double gap = now - g_tmf_seen_ms.exchange(now);
    TmfState& s = g_tmf;
    if (s.self != self || s.list != list) {   // a new title page: its tags are new (and visible)
        s = TmfState{};
        s.self = self;
        s.list = list;
        s.page = ++g_pages;
        if (s.page == 1) {
            g_first_page_ms.store(now);
            const double skipped = g_skip_ms.load();
            if (skipped > 0) Log("[title] the first title page is up %.1f s after the boot logos were skipped", (now - skipped) / 1000.0);
        }
    } else if (gap > kPageGapMs && (s.resolved || s.n_hide)) {
        Revalidate(self, list, s, gap);
    }
    const bool want = Want();
    if (want) {
        if (!s.resolved) Resolve(self, list, s);
        int now_hidden = 0;
        for (int i = 0; i < s.n_hide; ++i) {
            // only a tag that is still a child of this page's list (never a cached address alone: a freed page's tag)
            if (!IsChild(list, s.hide[i])) continue;
            std::uint8_t vis = 0;
            if (!mem::Rd(s.hide[i] + kTagVisible, vis)) continue;
            if (vis && CallSetVisible(s.hide[i], false)) {
                if (!s.hidden[i]) g_hidden_count.fetch_add(1);
                s.hidden[i] = true;
            }
            if (s.hidden[i]) ++now_hidden;
        }
        if (!s.told && now_hidden) {
            s.told = true;
            char names[200] = "";
            std::size_t at = 0;
            for (int i = 0; i < s.n_hide; ++i)
                if (s.hidden[i] && at < sizeof(names))
                    at += static_cast<std::size_t>(std::snprintf(names + at, sizeof(names) - at, "%s%s", at ? ", " : "", s.hide_names[i]));
            Log("[title] native waiting title ON (title page #%u): NEW GAME carries the label; CONTINUE / LOAD GAME / PS4->PS5 hidden by "
                "the data source; hidden: %s; EXIT GAME stays",
                s.page, names);
        }
        return;
    }
    // The game's menu back: the items this page hid, still children of its list.
    bool any = false;
    for (int i = 0; i < s.n_hide; ++i) {
        if (!s.hidden[i]) continue;
        s.hidden[i] = false;
        if (IsChild(list, s.hide[i]) && CallSetVisible(s.hide[i], true)) any = true;
    }
    if (any) Log("[title] the hidden title menu items are visible again");
    s.told = false;
}

// ---------------------------------------------------------------- the boot logos: the splash's own skip-all branch
int SplashSkipSeh(std::uintptr_t tag, std::uintptr_t menu_sys) noexcept {
    __try {
        reinterpret_cast<FnSetFocus>(A(kSetFocus))(tag, tag, 0);
        reinterpret_cast<FnPageFinished>(A(kPageFinished))(menu_sys);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// Pre-hook on the splash page's Update (the game's menu thread). Only its very first update after OnPageOn (state 0, next
// state 1: before the settings dialog and the first logo), no settings-reset dialog pending (that flow stays the
// game's), once per game: what the game's own branch does, then return without the original. Anything else: the original.
void H_splash(std::uintptr_t self, float dt) {
    if (g_skip_armed.load()) {
        std::uintptr_t vt = 0, tag = 0, menu_sys = 0;
        std::uint8_t state = 0xFF, next = 0xFF, dialog = 1;
        if (mem::Rd(self, vt) && vt == A(kSplashVtable) && mem::Rd(self + kSplashState, state) && mem::Rd(self + kSplashNext, next)) {
            g_skip_armed.store(false);   // decided at the splash's first update, whatever the outcome
            const bool first = state == 0 && next == 1;
            const bool ok = first && mem::Rd(A(kSplashDialogFlag), dialog) && dialog == 0 && mem::Rd(self + kSplashEndTag, tag) && tag &&
                            mem::Rd(A(kMenuSysGlobal), menu_sys) && menu_sys;
            if (ok) {
                const int r = SplashSkipSeh(tag, menu_sys);
                g_skip_result.store(r);
                if (r == 1) {
                    g_skip_ms.store(link::NowMsPrecise());
                    Log("[title] boot logos skipped (the splash page's own skip-all path): the title page comes next");
                    return;
                }
                Log("[title] FAULT in the boot-logo skip: the logos play");
            } else {
                g_skip_result.store(-2);
                Log("[title] boot logos not skipped (splash state %u, next %u, settings dialog %u, end tag %s): the logos play",
                    state, next, dialog, tag ? "set" : "missing");
            }
        }
    }
    o_splash(self, dt);
}

// ---------------------------------------------------------------- the start handlers: nothing while the guest waits
void Ignored(const char* what) noexcept {
    const unsigned n = g_ignored.fetch_add(1) + 1;
    if (n <= 20 || n % 50 == 0) Log("[title] %s pressed on the waiting title: ignored #%u (the guest waits for the host)", what, n);
}
void H_newgame(std::uintptr_t self) {
    if (Want()) return Ignored("the label (NEW GAME)");
    o_newgame(self);
}
void H_continue(std::uintptr_t self) {
    if (Want()) return Ignored("CONTINUE");
    o_continue(self);
}
void H_loadslot(std::uintptr_t self) {
    if (Want()) return Ignored("LOAD GAME");
    o_loadslot(self);
}

bool Hook(std::uintptr_t va, void* detour, void** orig, const char* name, char* why, std::size_t cap) noexcept {
    void* target = reinterpret_cast<void*>(A(va));
    MH_STATUS st = MH_CreateHook(target, detour, orig);
    if (st == MH_OK) st = MH_EnableHook(target);
    if (st != MH_OK) {
        std::snprintf(why, cap, "the %s hook failed (%s)", name, MH_StatusToString(st));
        return false;
    }
    return true;
}

// The boot-logo skip (Install, after the title's own hooks): its own byte checks; a mismatch leaves only the skip off.
void InstallSkip() noexcept {
    const char* bad = nullptr;
    for (const Site& s : kSkipSites)
        if (!mem::CodeMatches(A(s.va), s.sig)) {
            bad = s.name;
            break;
        }
    if (!bad && mem::RipTarget(A(kSplashSkipAll), 3, 7) != A(kSplashPlatform)) bad = "splash skip-all branch (the platform object)";
    if (!bad && mem::RipTarget(A(kSplashSkipMenuSys), 3, 7) != A(kMenuSysGlobal)) bad = "splash skip-all branch (the menu system)";
    if (!bad && mem::RipTarget(A(kSplashDialogRead), 2, 7) != A(kSplashDialogFlag)) bad = "splash settings-dialog flag (its address)";
    if (!bad && mem::Q(A(kSplashVtable) + 14 * 8) != A(kSplashUpdate)) bad = "the splash page's vtable";
    if (bad) {
        Log("[title] boot-logo skip OFF: this game build differs (%s byte check failed); the logos play", bad);
        return;
    }
    char why[120] = "";
    if (!Hook(kSplashUpdate, reinterpret_cast<void*>(&H_splash), reinterpret_cast<void**>(&o_splash), "splash Update", why, sizeof(why))) {
        Log("[title] boot-logo skip OFF: %s; the logos play", why);
        return;
    }
    g_skip_armed.store(true);
    Log("[title] boot-logo skip armed: %u byte checks ok (the splash page's own skip-all path at its first update)",
        static_cast<unsigned>(sizeof(kSkipSites) / sizeof(kSkipSites[0])));
}

}  // namespace

bool Install(char* why, std::size_t cap, bool skip_logos) noexcept {
    why[0] = 0;
    for (const Site& s : kSites)
        if (!mem::CodeMatches(A(s.va), s.sig)) {
            std::snprintf(why, cap, "this game build differs (%s byte check failed)", s.name);
            Log("[title] byte check FAILED: %s at 0x%llX", s.name, static_cast<unsigned long long>(s.va));
            break;
        }
    if (!why[0] && mem::RipTarget(A(kTmfInitMenuSys), 3, 7) != A(kMenuSysGlobal)) {
        std::snprintf(why, cap, "this game build differs (the menu system global)");
        Log("[title] byte check FAILED: title menu Init does not name the menu system 0x%llX", static_cast<unsigned long long>(kMenuSysGlobal));
    }
    if (!why[0] && mem::RipTarget(A(kUiGlobalsRead), 3, 7) != A(kUiGlobals)) {
        std::snprintf(why, cap, "this game build differs (the UI globals)");
        Log("[title] byte check FAILED: GetUIGlobals does not name 0x%llX", static_cast<unsigned long long>(kUiGlobals));
    }
    if (!why[0] && (mem::Q(A(kDsVtable) + 5 * 8) != A(kDsUpdate) || mem::Q(A(kTmfVtable) + 13 * 8) != A(kTmfInit) ||
                    mem::Q(A(kTmfVtable) + 14 * 8) != A(kTmfUpdate))) {
        std::snprintf(why, cap, "this game build differs (the title menu's vtables)");
        Log("[title] byte check FAILED: the title menu vtables (DS 0x%llX vt[5], TMF 0x%llX vt[13]/vt[14])",
            static_cast<unsigned long long>(kDsVtable), static_cast<unsigned long long>(kTmfVtable));
    }
    if (why[0]) {
        Log("[title] the guest's title in the game's own menu is OFF (%s): the full-screen waiting panel is used", why);
        return false;
    }
    // The handlers first: once the label is shown, pressing it must do nothing.
    if (!Hook(kOnNewGame, reinterpret_cast<void*>(&H_newgame), reinterpret_cast<void**>(&o_newgame), "OnNewGame", why, cap) ||
        !Hook(kOnContinue, reinterpret_cast<void*>(&H_continue), reinterpret_cast<void**>(&o_continue), "OnContinueGame", why, cap) ||
        !Hook(kOnLoadSlot, reinterpret_cast<void*>(&H_loadslot), reinterpret_cast<void**>(&o_loadslot), "OnSelectLoadSlot", why, cap) ||
        !Hook(kDsUpdate, reinterpret_cast<void*>(&H_ds), reinterpret_cast<void**>(&o_ds), "title data source Update", why, cap) ||
        !Hook(kTmfUpdate, reinterpret_cast<void*>(&H_tmf), reinterpret_cast<void**>(&o_tmf), "title menu Update", why, cap)) {
        // A hook that is in stays harmless: g_installed is false, every detour passes through.
        Log("[title] the guest's title in the game's own menu is OFF (%s): the full-screen waiting panel is used", why);
        return false;
    }
    g_installed = true;
    Log("[title] the guest's title in the game's own menu: %u byte checks ok, hooks in (title data source / menu Update, "
        "OnNewGame / OnContinueGame / OnSelectLoadSlot); EXIT GAME untouched",
        static_cast<unsigned>(sizeof(kSites) / sizeof(kSites[0])));
    if (skip_logos) InstallSkip();
    else Log("[title] boot-logo skip off (guest_skip_logos=0): the logos play");
    return true;
}

bool Installed() noexcept { return g_installed; }

bool PageSeen() noexcept { return g_first_page_ms.load() > 0; }

void Set(bool want, const char* label) noexcept {
    if (!g_installed) return;
    char next[kLabelCap] = {};
    std::snprintf(next, sizeof(next), "%s", label ? label : "");
    if (want && next[0]) {
        AcquireSRWLockExclusive(&g_mx);
        std::memcpy(g_label, next, kLabelCap);
        ReleaseSRWLockExclusive(&g_mx);
    }
    const bool was = g_want.exchange(want && next[0]);
    if (want && next[0] && std::strcmp(next, g_logged_label) != 0) {
        // The transfer's percentage at most once a second in the log (the menu shows every step).
        static double last_ms = -1e18;
        const double now = link::NowMsPrecise();
        const bool progress = std::strncmp(next, "Receiving", 9) == 0 && std::strncmp(g_logged_label, "Receiving", 9) == 0;
        if (!progress || now - last_ms >= 1000) {
            last_ms = now;
            std::memcpy(g_logged_label, next, kLabelCap);
            Log("[title] label: %s", next);
        }
    }
    if (was && !(want && next[0])) {
        g_logged_label[0] = 0;
        Log("[title] the session wants the game's own title menu back (restored at its next update)");
    }
}

bool Showing(double now_ms) noexcept {
    return Want() && g_label_on.load() && now_ms - g_tmf_seen_ms.load() < 1500;
}

bool LabelMissing(double now_ms) noexcept {
    return g_installed && Want() && !g_label_on.load() && now_ms - g_tmf_seen_ms.load() < 1500;
}

unsigned Faults() noexcept { return g_faults.load(); }

void Describe(char* out, std::size_t cap) noexcept {
    const double now = link::NowMsPrecise();
    const int sk = g_skip_result.load();
    std::snprintf(out, cap, "native title %s; wanted %d, label in the menu %d, menu update %.1f s ago, items hidden %u, presses ignored %u, faults %u; "
                  "boot logos %s",
                  g_installed ? "installed" : "OFF", g_want.load() ? 1 : 0, g_label_on.load() ? 1 : 0, (now - g_tmf_seen_ms.load()) / 1000.0,
                  g_hidden_count.load(), g_ignored.load(), g_faults.load(),
                  sk == 1 ? "skipped" : sk == -1 ? "skip faulted" : sk == -2 ? "not skipped" : g_skip_armed.load() ? "skip armed" : "played");
}

}  // namespace st::title_wait
