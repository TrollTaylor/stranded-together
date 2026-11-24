#include "wardrobe.hpp"

#include <windows.h>

#include <cstdio>

#include "log.hpp"
#include "partner_source.hpp"

namespace st::wardrobe {
namespace {

namespace mem = partner::mem;
using mem::Rd;

// ---------------------------------------------------------------- ds.exe (RVAs at base 0x140000000)
constexpr std::uintptr_t kOpenTest = 0x2D1BD83;     // cmp byte [rip->flag],0; jne: the opener 0x142D1BD70
constexpr std::uintptr_t kOpenSet = 0x2D1BD8C;      // mov byte [rip->flag],1
constexpr std::uintptr_t kOpenCopyCall = 0x2D1BD93; // call 0x14284CB30 (inventory -> staging)
constexpr std::uintptr_t kOpenCopy = 0x284CB30;
constexpr std::uintptr_t kCopyRead = 0x284CB7A;     // movzx r10d,[rax+0x4BE1]
constexpr std::uintptr_t kCopyStore = 0x284CBC3;    // vmovss [rip->staging],xmm1
constexpr std::uintptr_t kClose = 0x2D1BCE0;        // sub rsp,28; call write-back; mov byte [rip->flag],0; add rsp,28; ret
constexpr std::uintptr_t kWriteBack = 0x283E5B0;
constexpr std::uintptr_t kWriteBackRead = 0x283E603;   // vcvttss2si ecx,[rip->staging]
constexpr std::uintptr_t kWriteBackStore = 0x283E62D;  // mov [rax+0x4BE1],cl
constexpr std::uintptr_t kPreviewRead = 0x272EAAF;     // mov rax,[rip->category]; cmp al,3; jne; movzx ebx,byte [rip->value] ...
constexpr std::uintptr_t kKindBranch = 0x272A16E;      // cmp ecx,6; jne; movzx eax,byte [rip->category]; cmp al,8
// The UI object the opener posts its event to (0x142D1BD98: mov rcx,[rip->it]), also read by the private room's state
// machine 0x142763820.
constexpr std::uintptr_t kRoom = 0x7C0D6C0;
constexpr std::uintptr_t kFlag = 0x4C46990, kStaging = 0x4C46988, kCategory = 0x4C46991, kValue = 0x4C46994;

struct Sites {
    bool done = false, ok = false;
    char why[200] = "not checked";
} g;

bool Check(const char* name, std::uintptr_t rva, const char* sig) {
    if (mem::CodeMatches(mem::Base() + rva, sig)) return true;
    std::snprintf(g.why, sizeof(g.why), "%s at ds.exe+0x%llX does not match", name, static_cast<unsigned long long>(rva));
    return false;
}

}  // namespace

bool SitesOk() noexcept {
    if (g.done) return g.ok;
    g.done = true;
    const std::uintptr_t b = mem::Base();
    bool ok = Check("uniform menu open test", kOpenTest, "80 3D ?? ?? ?? ?? 00 75 3B") && Check("uniform menu open flag", kOpenSet, "C6 05 ?? ?? ?? ?? 01") &&
              Check("uniform menu staging copy call", kOpenCopyCall, "E8") &&
              Check("staging copy: the suit colour read", kCopyRead, "44 0F B6 90 E1 4B 00 00") &&
              Check("staging copy: the suit colour store", kCopyStore, "C5 FA 11 0D ?? ?? ?? ??") &&
              Check("uniform menu close", kClose, "48 83 EC 28 E8 ?? ?? ?? ?? C6 05 ?? ?? ?? ?? 00 48 83 C4 28 C3") &&
              Check("write-back: the suit colour read", kWriteBackRead, "C5 FA 2C 0D ?? ?? ?? ??") &&
              Check("write-back: the suit colour store", kWriteBackStore, "88 88 E1 4B 00 00") &&
              Check("figure preview colour read", kPreviewRead,
                    "48 8B 05 ?? ?? ?? ?? 3C 03 75 21 0F B6 1D ?? ?? ?? ?? 48 8B 4C 24 68 0F B6 D3 E8") &&
              Check("figure kind-6 branch", kKindBranch, "83 F9 06 75 39 0F B6 05 ?? ?? ?? ?? 3C 08");
    if (ok && (mem::RipTarget(b + kOpenTest, 2, 7) != b + kFlag || mem::RipTarget(b + kOpenSet, 2, 7) != b + kFlag ||
               mem::RipTarget(b + kOpenCopyCall, 1, 5) != b + kOpenCopy || mem::RipTarget(b + kCopyStore, 4, 8) != b + kStaging ||
               mem::RipTarget(b + kClose + 4, 1, 5) != b + kWriteBack || mem::RipTarget(b + kClose + 9, 2, 7) != b + kFlag ||
               mem::RipTarget(b + kWriteBackRead, 4, 8) != b + kStaging || mem::RipTarget(b + kPreviewRead, 3, 7) != b + kCategory ||
               mem::RipTarget(b + kPreviewRead + 11, 3, 7) != b + kValue || mem::RipTarget(b + kKindBranch + 5, 3, 7) != b + kCategory)) {
        std::snprintf(g.why, sizeof(g.why), "the uniform menu's flag / staging / preview references do not lead where expected");
        ok = false;
    }
    if (ok) std::snprintf(g.why, sizeof(g.why), "ok");
    g.ok = ok;
    Log("[wardrobe] the uniform menu %s%s", ok ? "readable (open flag, staging suit colour, preview category and value verified)" : "unreadable: ",
        ok ? "" : g.why);
    return ok;
}

const char* Why() noexcept { return g.why; }

bool Read(State* s) noexcept {
    *s = State{};
    if (!SitesOk()) return false;
    const std::uintptr_t b = mem::Base();
    std::uint8_t flag = 0xFF, cat = 0xFF, val = 0xFF;
    float st = -1.0f;
    if (!Rd(b + kFlag, flag) || !Rd(b + kCategory, cat) || !Rd(b + kValue, val) || !Rd(b + kStaging, st)) return false;
    s->open = flag == 1 ? 1 : flag == 0 ? 0 : -1;
    s->category = cat;
    s->value = val;
    s->staging = st >= -0.25f && st <= 16.25f ? static_cast<int>(st + 0.5f) : -1;
    return true;
}

bool TestOpen(char* msg, std::size_t cap) noexcept {
    const std::uintptr_t b = mem::Base();
    if (!SitesOk()) {
        std::snprintf(msg, cap, "the uniform menu is not readable: %s", g.why);
        return false;
    }
    constexpr std::uintptr_t kOpener = 0x2D1BD70, kOpenerUi = 0x2D1BD98, kOpenerPost = 0x2D1BDC2;
    if (!mem::CodeMatches(b + kOpener, "48 83 EC 58 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 40 80 3D") ||
        !mem::CodeMatches(b + kOpenerUi, "48 8B 0D ?? ?? ?? ?? 48 8D 54 24 20") || mem::RipTarget(b + kOpenerUi, 3, 7) != b + kRoom ||
        !mem::CodeMatches(b + kOpenerPost, "E8")) {
        std::snprintf(msg, cap, "the opener 0x142D1BD70 does not match: not called");
        return false;
    }
    std::uint8_t flag = 0xFF;
    std::uintptr_t ui = 0;
    if (!Rd(b + kFlag, flag) || flag != 0 || !Rd(b + kRoom, ui) || !ui) {
        std::snprintf(msg, cap, "not called: the menu flag is %u, the UI object %p", flag, reinterpret_cast<void*>(ui));
        return false;
    }
    bool ok = false;
    __try {
        reinterpret_cast<void (*)()>(b + kOpener)();
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    Rd(b + kFlag, flag);
    std::snprintf(msg, cap, "TEST: the uniform menu's opener 0x142D1BD70 %s (flag now %u)", ok ? "called" : "FAULTED", flag);
    Log("[wardrobe] %s", msg);
    return ok;
}

}  // namespace st::wardrobe
