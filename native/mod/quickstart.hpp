// Hidden runs' quickstart (docs/hidden-testing.md "start"): the scenario's `start` line makes the supervisor write
// automation_quickstart=<spec> into session.ini (never the launcher, never a scenario "ini" line), and this game then takes
// itself from the title into the world with the call the title menu's own CONTINUE / LOAD GAME make, without a single menu
// step in the scenario:
//
//   continue          Application.ContinueLastSave 0x142642A00 (the session global 0: the title load 0x142624A70 with
//                     (app, 0xF, -1), the newest save; the same function the title's CONTINUE ends in, so a host-held save
//                     host's coop hook on it offers that save to the guest as when A is pressed)
//   load <type> <i>   the title load 0x142624A70(app [0x147BE6330], type, i): type 1 manualsave<i>, 4 autosave<i>, ...
//   story <n>         load 1 <n> (the story library's manualsave10..19)
//
// The call goes out once the title page has been up for 3 s (title_wait::TitlePageUpMs: the rule the host-held save guest's
// own title load uses), on the game thread (dev::Tick). A host-held save guest makes no call: the host's save loads it. Then
// "[quick] in the world after ..." once the local Sam is playable and still for 1.5 s (the scenario's `waitworld` waits for
// the same). Every address is byte-checked at start-up; a mismatch leaves the quickstart off with one log line (the
// scenario's waitworld then fails: nothing is pressed blind).
//
// Only with automation=1 in a game the supervisor started (ST_TESTENV_SUPERVISED) whose saves are redirected: a hidden run.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace st {
struct Config;
}

namespace st::quickstart {

// ---------------------------------------------------------------- the spec (pure; tests/testenv)
struct Spec {
    enum Mode : std::uint8_t { none, continue_last, load } mode = none;
    int type = 0;   // load: the game's slot type (1 manual, 2 quick, 4 auto)
    int index = 0;  // load: the slot index
};
// "continue", "load <type 1..15> <index 0..127>", "story <n 0..127>" (= load 1 n); "" = none. False on anything else.
inline bool ParseSpec(const char* text, Spec* out) {
    *out = Spec{};
    if (!text || !text[0]) return true;
    char word[16] = {};
    int a = -1, b = -1, n = 0;
    const int got = std::sscanf(text, "%15s %d %d%n", word, &a, &b, &n);
    if (std::strcmp(word, "continue") == 0) {
        char extra[4] = {};
        if (std::sscanf(text, "%15s %3s", word, extra) != 1) return false;
        out->mode = Spec::continue_last;
        return true;
    }
    if (std::strcmp(word, "load") == 0 && got == 3 && a >= 1 && a <= 15 && b >= 0 && b <= 127 && text[n] == 0) {
        out->mode = Spec::load, out->type = a, out->index = b;
        return true;
    }
    if (std::strcmp(word, "story") == 0 && got == 2 && a >= 0 && a <= 127) {
        char extra[4] = {};
        int x = 0;
        if (std::sscanf(text, "%15s %d %3s", word, &x, extra) != 2) return false;
        out->mode = Spec::load, out->type = 1, out->index = a;
        return true;
    }
    return false;
}

// Bootstrap thread, after coop::Install (its title hooks first) and before the overlay waits for the game's d3d12 (the boot-logo
// skip must be in before the splash's first update). No-op without automation_quickstart.
void Install(const Config& config) noexcept;
// Game thread, every tick (dev::Tick): the title call, the world wait, the log lines.
void Tick(double now_ms) noexcept;

}  // namespace st::quickstart
