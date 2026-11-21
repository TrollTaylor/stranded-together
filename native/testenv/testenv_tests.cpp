// st_testenv_tests: checks of the hidden test environment without the game.
//
//   st_testenv_tests [--scenarios <dir>]
//       pure checks: the mod's automation command parser, the scenario language
//       (and every scenario file in <dir>), session.ini text, save manifests, and
//       backup/restore of a scratch "game folder"
//   st_testenv_tests --e2e --supervisor <exe> --fakegame <exe> --mod <winhttp.dll>
//                    --scenario <file> --work <dir>
//       end to end on a private desktop: a scratch game folder with the fake game
//       as ds.exe, the real mod, the real supervisor; checks screenshots (decoded),
//       the virtual pad reaching the "game", quit, and that the folder was restored.
//       Exit 77 (skipped) when a ds.exe is already running.
#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "automation.hpp"
#include "sandbox.hpp"
#include "quickstart.hpp"
#include "testenv_core.hpp"

namespace te = st::testenv;
namespace au = st::automation;

namespace {

int g_fail = 0, g_pass = 0;
void Check(bool ok, const char* what, const std::string& detail = {}) {
    std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : "  -- ", detail.c_str());
    std::fflush(stdout);
    (ok ? g_pass : g_fail)++;
}

bool Parse(const char* line, au::Command* c = nullptr) {
    au::Command tmp;
    std::string err;
    return au::ParseCommand(line, c ? c : &tmp, &err);
}

void CommandParser() {
    au::Command c;
    Check(Parse("wait 1500", &c) && c.kind == au::Kind::wait && c.ms == 1500, "wait <ms>");
    Check(!Parse("wait -5") && !Parse("wait") && !Parse("wait 9999999"), "wait rejects bad times");
    Check(Parse("screenshot 01-title_a.b", &c) && c.kind == au::Kind::screenshot && c.text == "01-title_a.b", "screenshot <name>");
    Check(!Parse("screenshot ../x") && !Parse("screenshot .hidden") && !Parse("screenshot a/b"), "screenshot names cannot leave out\\");
    Check(Parse("pad A+START 250 lx=-32768 ry=32767 rt=255", &c) && c.kind == au::Kind::pad &&
              c.buttons == (au::kPadA | au::kPadStart) && c.ms == 250 && c.lx == -32768 && c.ry == 32767 && c.rt == 255,
          "pad buttons, hold, axes");
    Check(Parse("pad none 100", &c) && c.buttons == 0 && Parse("PAD dpad_down 50", &c) && c.buttons == au::kPadDown, "pad none / case");
    Check(!Parse("pad Q 100") && !Parse("pad A") && !Parse("pad A 100 lx=40000") && !Parse("pad A 100 lt=-1") && !Parse("pad A 100 zz=1"),
          "pad rejects unknown buttons / bad axes");
    Check(Parse("pad none 0 ly=32767 keep", &c) && c.keep && c.ly == 32767 && Parse("pad none 100", &c) && !c.keep && !Parse("pad none 100 keep ly=1"),
          "pad keep (last word only)");
    Check(Parse("key F9 100", &c) && c.vk == 0x78 && Parse("key esc 10", &c) && c.vk == 0x1B && Parse("key w 10", &c) && c.vk == 'W' &&
              Parse("key 0x41 10", &c) && c.vk == 0x41,
          "key names");
    Check(!Parse("key F13 10") && !Parse("key nope 10"), "key rejects unknown keys");
    Check(Parse("menu open", &c) && c.text == "open" && !Parse("menu maybe"), "menu open|close|toggle");
    Check(Parse("action teleport 1 2.5 -3", &c) && c.kind == au::Kind::action && c.text == "teleport" && c.numbers.size() == 3 &&
              c.numbers[1] == 2.5,
          "action with numbers");
    Check(!Parse("action teleport x") && !Parse("action t 1 2 3 4"), "action rejects non-numbers / too many");
    Check(Parse("waitplayer 120", &c) && c.ms == 120000 && Parse("waitlog 30 [sync] local player", &c) &&
              c.text == "[sync] local player" && c.ms == 30000,
          "waitplayer / waitlog keep the text with spaces");
    Check(Parse("log hello world", &c) && c.text == "hello world" && Parse("quit", &c) && c.kind == au::Kind::quit &&
              Parse("snapshot", &c) && Parse("padoff", &c),
          "log / quit / snapshot / padoff");
    Check(Parse("quit pad", &c) && c.kind == au::Kind::quit && c.text == "pad" && Parse("quit", &c) && c.text.empty() && !Parse("quit now") &&
              !Parse("quit pad 1"),
          "quit pad: the game's own menu quits");
    std::string err;
    Check(!au::ParseCommand("   # comment", &c, &err) && err.empty() && !au::ParseCommand("", &c, &err) && err.empty(),
          "blank and comment lines are not errors");
    Check(!au::ParseCommand("frobnicate", &c, &err) && !err.empty(), "unknown command is an error", err);
    Check(au::NormalizeActionName("Respawn partner") == "respawn_partner" && au::NormalizeActionName("LOG-status ") == "log_status" &&
              au::NormalizeActionName("Teleport to partner") == "teleport_to_partner",
          "action names: label <-> snake case");
    // session.ini saved as "UTF-8 with BOM": its first key reads as itself in every reader (DllMain's EarlyInit, Init).
    Check(au::detail::StripBom("\xEF\xBB\xBFrole=guest") == "role=guest" && au::detail::StripBom("role=guest") == "role=guest" &&
              au::detail::StripBom("\xEF\xBB") == "\xEF\xBB" && au::detail::StripBom("\xEFrole") == "\xEFrole",
          "session.ini: a UTF-8 BOM is stripped from the first line only when it is all three bytes");
}

void ScenarioParser(const std::wstring& dir) {
    te::Scenario s;
    std::string err;
    const char* good =
        "# smoke\nrole echo\ntimeout 300\nwaitready 60\nwaitlog 90 \\[sync\\] first tick\n"
        "until 200 3000 local player entity 0+ -> 0*[1-9A-F] => pad A 150\n?screenshot x\nsleep 500\nnote hi there\nquit\n";
    Check(te::ParseScenario(good, &s, &err) && s.role == "echo" && s.timeout_s == 300 && s.steps.size() == 7, "scenario parses", err);
    if (s.steps.size() == 7) {
        Check(s.steps[0].kind == te::StepKind::waitready && s.steps[0].seconds == 60, "waitready step");
        Check(s.steps[1].kind == te::StepKind::waitlog && s.steps[1].text == "\\[sync\\] first tick", "waitlog keeps the regex");
        Check(s.steps[2].kind == te::StepKind::until && s.steps[2].ms == 3000 && s.steps[2].command == "pad A 150" &&
                  s.steps[2].text == "local player entity 0+ -> 0*[1-9A-F]",
              "until: regex and command split at =>");
        Check(s.steps[3].optional && s.steps[3].kind == te::StepKind::mod, "? marks a step optional");
        Check(s.steps[6].kind == te::StepKind::mod && s.steps[6].text == "quit", "mod commands pass through");
    }
    Check(!te::ParseScenario("timeout 999\nquit\n", &s, &err), "timeout above 360 s refused", err);
    Check(te::ParseScenario("role host\nnolog back to the real body\n@guest nolog x\nquit\n", &s, &err) && s.steps.size() == 3 &&
              s.steps[0].kind == te::StepKind::nolog && s.steps[0].text == "back to the real body" && s.steps[1].target == 1 &&
              s.steps[1].kind == te::StepKind::nolog,
          "nolog parses (also for the guest)", err);
    Check(!te::ParseScenario("nolog\nquit\n", &s, &err) && !te::ParseScenario("nolog (\nquit\n", &s, &err) &&
              !te::ParseScenario("role host\n@both nolog x\nquit\n", &s, &err),
          "nolog needs a valid regex and no @both", err);
    Check(te::ParseScenario("ini partner_body=real\nini echo_delay_s = 2.0\nquit\n", &s, &err) && s.ini.size() == 2 &&
              s.ini[0] == "partner_body=real" && s.ini[1] == "echo_delay_s=2.0",
          "ini lines parse", err);
    Check(!te::ParseScenario("ini automation_save_root=C:\\x\nquit\n", &s, &err), "ini refuses keys outside the allow-list", err);
    Check(!te::ParseScenario("ini partner_body\nquit\n", &s, &err), "ini needs key=value", err);
    Check(te::ParseScenario("role host\nini partner_body=real\n@host ini my_character=sam\n@guest ini my_character=fragile\n"
                            "@guest INI partner_body=holo\n@guest quit\nquit\n",
                            &s, &err) &&
              s.ini.size() == 1 && s.host_ini.size() == 1 && s.host_ini[0] == "my_character=sam" && s.guest_ini.size() == 2 &&
              s.guest_ini[0] == "my_character=fragile" && s.guest_ini[1] == "partner_body=holo" && s.steps.size() == 2,
          "@host ini / @guest ini go to one game", err);
    Check(!te::ParseScenario("@guest ini my_character=fragile\nquit\n", &s, &err), "@guest ini needs role host", err);
    Check(!te::ParseScenario("role host\n@both ini partner_body=real\nquit\n", &s, &err), "@both ini refused (plain ini is both)", err);
    Check(!te::ParseScenario("role host\n@guest ini automation=1\nquit\n", &s, &err), "@guest ini keeps the allow-list", err);
    Check(!te::ParseScenario("role host\n?@guest ini my_character=fragile\nquit\n", &s, &err), "ini cannot be optional", err);
    {
        te::SessionOptions so;
        so.role = "echo";
        so.extra = {"partner_body=real"};
        Check(te::SessionIniText(so).find("\npartner_body=real\n") != std::string::npos, "ini lines end up in session.ini", "");
    }
    {
        // frame_cap from a scenario: written after the supervisor's own (graphics low: 30), and the mod
        // keeps a key's last value, so the scenario's wins; smooth_pose is a scenario key too.
        Check(te::ParseScenario("role host\ngraphics low\nini frame_cap=60\nini smooth_pose=0\nquit\n", &s, &err) && s.ini.size() == 2 &&
                  s.ini[0] == "frame_cap=60" && s.ini[1] == "smooth_pose=0",
              "ini frame_cap / smooth_pose allowed", err);
        te::SessionOptions so;
        so.role = "host";
        so.frame_cap = 30;
        so.extra = {"frame_cap=60"};
        const std::string t = te::SessionIniText(so);
        Check(t.find("frame_cap=30\n") != std::string::npos && t.find("frame_cap=60\n") != std::string::npos &&
                  t.find("frame_cap=60\n") > t.find("frame_cap=30\n"),
              "a scenario frame_cap comes after the supervisor's", "");
    }
    Check(!te::ParseScenario("waitlog 10 ([\nquit\n", &s, &err), "bad regex refused", err);
    Check(!te::ParseScenario("until 10 1000 x => quit\n", &s, &err), "until cannot repeat quit", err);
    Check(!te::ParseScenario("pad Q 10\n", &s, &err), "bad mod command refused at parse time", err);
    Check(!te::ParseScenario("# nothing\n", &s, &err), "empty scenario refused");
    Check(te::AckTimeoutSeconds("wait 60000") == 75 && te::AckTimeoutSeconds("screenshot a") == 40 &&
              te::AckTimeoutSeconds("waitplayer 100") == 115,
          "ack timeouts follow the command");
    int files = 0;
    // the named places next to the scenarios (tools\testenv\places.txt), as Start-TestRun.ps1 hands them to the supervisor
    std::vector<te::Place> places;
    if (!dir.empty()) {
        std::string ptext, perr;
        const bool pok = te::ReadFileBytes(dir + L"\\..\\places.txt", &ptext) && te::ParsePlaces(ptext, &places, &perr);
        Check(pok && places.size() >= 4, "tools\\testenv\\places.txt parses", pok ? std::to_string(places.size()) + " places" : perr);
    }
    for (const std::wstring& rel : te::ListFiles(dir, false)) {
        if (rel.size() < 4 || rel.substr(rel.size() - 4) != L".txt") continue;
        std::string text;
        ++files;
        const bool ok = te::ReadFileBytes(dir + L"\\" + rel, &text) && te::ParseScenario(text, &s, &err, &places);
        Check(ok, ("scenario file " + te::Narrow(rel)).c_str(), ok ? std::to_string(s.steps.size()) + " steps" : err);
    }
    if (!dir.empty()) Check(files > 0, "scenario folder has scenarios", te::Narrow(dir));
}

// round 4: start (quickstart), goto (named places / coordinates), waitnew, the places table
void QuickstartGoto() {
    te::Scenario s;
    std::string err;
    std::vector<te::Place> pl;
    Check(te::ParsePlaces("# places\nmknot 200 -1100.09 270.64 352.49  # Mountain Knot City\nground-test 200 10 20 0\n", &pl, &err) && pl.size() == 2 &&
              pl[0].name == "mknot" && pl[0].area == 200 && pl[0].x == -1100.09 && pl[0].note == "Mountain Knot City" && pl[1].z == 0,
          "places: name area x y z # note", err);
    Check(!te::ParsePlaces("Bad_Name 200 1 2 3\n", &pl, &err) && !te::ParsePlaces("a 200 1 2\n", &pl, &err) &&
              !te::ParsePlaces("a 200 1 2 3\na 200 1 2 3\n", &pl, &err) && !te::ParsePlaces("a 200 99999 2 3\n", &pl, &err),
          "places: bad names, missing numbers, duplicates and out-of-range places refused");
    te::ParsePlaces("mknot 200 -1100.09 270.64 352.49\nhill 200 5 6 0\n", &pl, &err);
    // start: default continue, expands where it stands to waitready 90 + waitworld 120
    Check(te::ParseScenario("timeout 200\nstart\nscreenshot a\nquit\n", &s, &err) && s.quickstart == "continue" && s.steps.size() == 4 &&
              s.steps[0].kind == te::StepKind::waitready && s.steps[0].seconds == 90 && s.steps[0].line == 2 && s.steps[1].kind == te::StepKind::mod &&
              s.steps[1].text == "waitworld 120" && s.steps[1].target == 0 && s.steps[2].text == "screenshot a",
          "start: continue, waitready 90 + waitworld 120", err);
    Check(te::ParseScenario("start story 12 control 60\nquit\n", &s, &err) && s.quickstart == "story 12" && s.steps[1].text == "waitworld 60", "start story n control s", err);
    Check(te::ParseScenario("start load 1 5\nquit\n", &s, &err) && s.quickstart == "load 1 5", "start load type index", err);
    // a two-game run: both games (the role line may come after start)
    Check(te::ParseScenario("start\nrole host\nquit\n", &s, &err) && s.steps[0].target == 2 && s.steps[1].target == 2, "start: both games in a two-game run",
          err);
    Check(!te::ParseScenario("start\nstart\nquit\n", &s, &err) && !te::ParseScenario("start load 1\nquit\n", &s, &err) &&
              !te::ParseScenario("start load 0 1\nquit\n", &s, &err) && !te::ParseScenario("start foo\nquit\n", &s, &err) &&
              !te::ParseScenario("?start\nquit\n", &s, &err) && !te::ParseScenario("role host\n@guest start\nquit\n", &s, &err) &&
              !te::ParseScenario("start control 5\nquit\n", &s, &err),
          "start: twice, bad specs, optional, prefixed and a short control refused");
    // the quickstart key reaches session.ini (ours and the launcher's) and is never an allowed scenario ini key
    te::SessionOptions o;
    o.quickstart = "continue";
    Check(te::SessionIniText(o).find("\nautomation_quickstart=continue\n") != std::string::npos && !te::ScenarioIniKeyAllowed("automation_quickstart"),
          "start: automation_quickstart= written by the supervisor only");
    std::string hidden;
    std::vector<std::string> swapped;
    Check(te::HiddenIniFromLauncher("# Generated by Stranded Together\nrole=host\nhost_save=1\nsandbox_saves=local-host\n", o, &hidden, &swapped, &err) &&
              hidden.find("\nautomation_quickstart=continue\n") != std::string::npos,
          "start: launcherini files get automation_quickstart= too", err);
    // goto: a named place or coordinates -> the dev_goto action + waitworld goto; @guest / @both
    Check(te::ParseScenario("goto mknot\ngoto 1.5 -2 \ngoto 3 4 5.25\nquit\n", &s, &err, &pl) && s.steps.size() == 7 &&
              s.steps[0].text == "action dev_goto -1100.09 270.64 352.49" && s.steps[1].text == "waitworld 45 goto" &&
              s.steps[2].text == "action dev_goto 1.5 -2 0" && s.steps[4].text == "action dev_goto 3 4 5.25",
          "goto: place and coordinates", err);
    Check(te::ParseScenario("role host\n@guest goto hill\n?@both goto mknot\nquit\n", &s, &err, &pl) && s.steps.size() == 5 && s.steps[0].target == 1 &&
              s.steps[1].target == 1 && s.steps[0].text == "action dev_goto 5 6 0" && s.steps[2].target == 2 && s.steps[3].optional,
          "goto: @guest / @both / optional", err);
    Check(!te::ParseScenario("goto nowhere\nquit\n", &s, &err, &pl) && !te::ParseScenario("goto mknot\nquit\n", &s, &err) &&
              !te::ParseScenario("goto 1 2 3 4\nquit\n", &s, &err, &pl) && !te::ParseScenario("goto 1e9 2\nquit\n", &s, &err, &pl),
          "goto: unknown place, no table, too many numbers, out of range refused");
    // waitnew
    Check(te::ParseScenario("role host\nwaitnew 10 \\[dev\\] goto\n@guest waitnew 5 x\nquit\n", &s, &err) && s.steps[0].kind == te::StepKind::waitnew &&
              s.steps[0].seconds == 10 && s.steps[0].text == "\\[dev\\] goto" && s.steps[1].target == 1,
          "waitnew", err);
    Check(!te::ParseScenario("waitnew 10 ([\nquit\n", &s, &err) && !te::ParseScenario("role host\n@both waitnew 5 x\nquit\n", &s, &err),
          "waitnew: bad regex, @both refused");
    // waitworld (the mod command)
    au::Command c;
    Check(au::ParseCommand("waitworld 30", &c, &err) && c.kind == au::Kind::waitworld && c.ms == 30000 && c.text.empty() &&
              au::ParseCommand("waitworld 45 goto", &c, &err) && c.text == "goto" && !au::ParseCommand("waitworld 0", &c, &err) &&
              !au::ParseCommand("waitworld 5 x", &c, &err) && te::AckTimeoutSeconds("waitworld 120") == 135,
          "waitworld <s> [goto]", err);
    // the quickstart spec the mod reads (quickstart.hpp)
    st::quickstart::Spec q;
    Check(st::quickstart::ParseSpec("continue", &q) && q.mode == st::quickstart::Spec::continue_last && st::quickstart::ParseSpec("load 1 5", &q) &&
              q.mode == st::quickstart::Spec::load && q.type == 1 && q.index == 5 && st::quickstart::ParseSpec("story 12", &q) && q.type == 1 && q.index == 12 &&
              st::quickstart::ParseSpec("", &q) && q.mode == st::quickstart::Spec::none && !st::quickstart::ParseSpec("load 1", &q) &&
              !st::quickstart::ParseSpec("load 0 1", &q) && !st::quickstart::ParseSpec("story 12 3", &q) && !st::quickstart::ParseSpec("continue now", &q) &&
              !st::quickstart::ParseSpec("newgame", &q),
          "quickstart spec: continue, load, story");
}

void SessionIni() {
    const std::string t = te::SessionIniText("solo", true);
    Check(t.find("role=solo\n") != std::string::npos && t.find("automation=1\n") != std::string::npos &&
              t.find("one_shot=1\n") != std::string::npos && t.find("transport=udp\n") != std::string::npos &&
              t.find("udp_peer=127.0.0.1\n") != std::string::npos && t.find("debug_menu=1\n") != std::string::npos &&
              t.find("automation_mute=1\n") != std::string::npos && t.find("secret") == std::string::npos,
          "session.ini: solo, loopback udp, automation, one-shot, no secret");
    Check(t.find("automation_save_root") == std::string::npos &&
              te::SessionIniText("solo", true, true, L"D:\\x\\saves").find("automation_save_root=D:\\x\\saves\n") != std::string::npos,
          "session.ini: automation_save_root only when a test root is given");
    const std::string r = te::RedirectOnlyIniText();
    Check(r.find("automation=1\n") != std::string::npos && r.find("one_shot=1\n") != std::string::npos && r.find("role=") == std::string::npos &&
              r.find("debug_menu") == std::string::npos,
          "--no-mod session.ini: automation (save redirect) only, no role, no menu");
}

// ds.exe's file bytes at an RVA (section table of the PE on disk). False when absent.
bool ReadDsExeRva(const std::string& pe, std::uint32_t rva, std::uint8_t* out, std::uint32_t len) {
    if (pe.size() < 0x400 || pe[0] != 'M' || pe[1] != 'Z') return false;
    const std::uint32_t nt = *reinterpret_cast<const std::uint32_t*>(pe.data() + 0x3C);
    if (nt + sizeof(IMAGE_NT_HEADERS64) > pe.size()) return false;
    const auto* h = reinterpret_cast<const IMAGE_NT_HEADERS64*>(pe.data() + nt);
    const auto* sec = IMAGE_FIRST_SECTION(h);
    for (WORD i = 0; i < h->FileHeader.NumberOfSections; ++i) {
        const IMAGE_SECTION_HEADER& s = sec[i];
        if (rva >= s.VirtualAddress && rva + len <= s.VirtualAddress + s.SizeOfRawData) {
            const std::size_t off = s.PointerToRawData + (rva - s.VirtualAddress);
            if (off + len > pe.size()) return false;
            std::memcpy(out, pe.data() + off, len);
            return true;
        }
    }
    return false;
}

// The foreign-saves bypass (automation.hpp "foreign saves"): the gate, the supervisor's rule,
// the byte check, and the site table against the installed ds.exe when there is one.
void ForeignSaves() {
    namespace fs = au::foreign_saves;
    using D = fs::Decision;
    Check(fs::Decide(false, true, true, true) == D::off && fs::Decide(false, false, false, false) == D::off,
          "foreign saves: off unless automation_foreign_saves=1 (normal play never patches)");
    Check(fs::Decide(true, false, true, true) == D::refuse_no_automation && fs::Decide(true, true, false, true) == D::refuse_no_automation &&
              fs::Decide(true, false, false, false) == D::refuse_no_automation,
          "foreign saves: refused outside a supervised automation run");
    Check(fs::Decide(true, true, true, false) == D::refuse_no_redirect, "foreign saves: refused when the save redirect is not active");
    Check(fs::Decide(true, true, true, true) == D::install, "foreign saves: installed only with automation + supervised + redirect + key");
    Check(te::WantForeignSaves(L"C:\\seed", false) && !te::WantForeignSaves(L"", false) && !te::WantForeignSaves(L"C:\\seed", true),
          "supervisor: the key only with --seed-saves and the full mod");
    const std::string plain = te::SessionIniText("solo", true), seeded = te::SessionIniText("echo", true, true, {}, true);
    Check(plain.find("automation_foreign_saves") == std::string::npos && seeded.find("automation_foreign_saves=1\n") != std::string::npos &&
              te::RedirectOnlyIniText().find("foreign") == std::string::npos,
          "session.ini: automation_foreign_saves=1 only when asked for");
    // site table sanity: jne rel8 at patch_at, distinct, inside .text of ds.exe (VA 0x140001000..0x143B3A000)
    bool table = fs::kSiteCount == 3;
    for (const fs::Site& s : fs::kSites)
        table = table && s.patch_at + 1 < s.len && s.expect[s.patch_at] == fs::kJne8 && s.expect[s.patch_at - 2] == 0x84 &&
                s.expect[s.patch_at - 1] == 0xDB && s.rva >= 0x1000 && s.rva + s.len < 0x3B3A000 && s.expect[0] == 0x48 &&
                s.expect[1] == 0x8B && s.expect[2] == 0x05;
    Check(table, "site table: each window loads the owner string and ends in test bl,bl; jne rel8");
    // owner string operand: mov rax,[rip+disp32] must resolve to 0x144598268 in every window
    bool owner = true;
    for (const fs::Site& s : fs::kSites) {
        std::int32_t disp = 0;
        std::memcpy(&disp, s.expect + 3, 4);
        owner = owner && fs::kImageBase + s.rva + 7 + disp == 0x144598268ull;
    }
    Check(owner, "site table: every window reads the account-id string at 0x144598268");
    // CheckSites with a synthetic image: all match -> true; one byte off -> false + where; unreadable -> false
    std::vector<std::uint8_t> image(0x3B3A000, 0xCC);
    for (const fs::Site& s : fs::kSites) std::memcpy(image.data() + s.rva, s.expect, s.len);
    const auto reader = [&](std::uint32_t rva, std::uint8_t* out, std::uint32_t len) {
        if (static_cast<std::size_t>(rva) + len > image.size()) return false;
        std::memcpy(out, image.data() + rva, len);
        return true;
    };
    std::string why;
    Check(fs::CheckSites(reader, &why), "byte check: matching image accepted", why);
    image[fs::kSites[1].rva + 50] ^= 0x01;
    why.clear();
    Check(!fs::CheckSites(reader, &why) && why.find("bytes differ at 0x") != std::string::npos && why.find("sync read") != std::string::npos,
          "byte check: one differing byte refuses (and names the site)", why);
    image[fs::kSites[1].rva + 50] ^= 0x01;
    image[fs::kSites[2].rva + fs::kSites[2].patch_at] = fs::kJmp8;  // already patched by someone else: refused too
    why.clear();
    Check(!fs::CheckSites(reader, &why), "byte check: an already patched site refuses", why);
    why.clear();
    Check(!fs::CheckSites([](std::uint32_t, std::uint8_t*, std::uint32_t) { return false; }, &why) && why.rfind("not readable", 0) == 0,
          "byte check: an unreadable image refuses", why);
    // The installed game, when present: the table matches this ds.exe build byte for byte.
    const std::wstring ds = L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\DEATH STRANDING DIRECTORS CUT\\ds.exe";
    std::string pe;
    if (te::ReadFileBytes(ds, &pe, 256ull << 20)) {
        why.clear();
        Check(fs::CheckSites([&](std::uint32_t rva, std::uint8_t* out, std::uint32_t len) { return ReadDsExeRva(pe, rva, out, len); }, &why),
              "byte check: the installed ds.exe matches every site", why);
    } else {
        std::printf("info  installed ds.exe not found: site table not checked against the game\n");
    }
}

// The mod's save redirect: path mapping (automation.hpp, also what the hooks run).
void SaveRedirectMapping() {
    namespace sv = au::saves;
    const std::wstring root = L"C:\\Users\\player\\AppData\\Local\\KojimaProductions";
    const std::wstring scratch = L"D:\\Games\\DS\\stranded_together\\automation\\saves";
    std::wstring out, rel;
    Check(sv::Map(root + L"\\DeathStrandingDC\\12345678\\autosave3\\checkpoint.dat", root, scratch, &out, &rel) &&
              out == scratch + L"\\DeathStrandingDC\\12345678\\autosave3\\checkpoint.dat" &&
              rel == L"DeathStrandingDC\\12345678\\autosave3\\checkpoint.dat",
          "a checkpoint under the root maps into the scratch copy", te::Narrow(out));
    Check(sv::Map(L"c:/users/PLAYER/appdata/local/kojimaproductions/DeathStrandingDC/12345678/profile/profile.dat", root, scratch, &out) &&
              out == scratch + L"\\DeathStrandingDC\\12345678\\profile\\profile.dat",
          "case and '/' separators do not matter (the rest keeps its spelling)", te::Narrow(out));
    Check(sv::Map(L"\\\\?\\C:\\Users\\player\\AppData\\Local\\KojimaProductions\\DeathStrandingDC\\1\\x", root, scratch, &out) &&
              out == L"\\\\?\\" + scratch + L"\\DeathStrandingDC\\1\\x",
          "a \\\\?\\ path maps and keeps its prefix", te::Narrow(out));
    Check(sv::Map(L"C:\\Users\\player\\AppData\\Local\\\\KojimaProductions\\\\DeathStrandingDC\\.\\1\\..\\2\\*", root, scratch, &out) &&
              out == scratch + L"\\DeathStrandingDC\\2\\*",
          "doubled separators, '.' and '..' are resolved before matching", te::Narrow(out));
    Check(sv::Map(root, root, scratch, &out, &rel) && out == scratch && rel.empty() && sv::Map(root + L"\\", root, scratch, &out) && out == scratch,
          "the root itself maps to the scratch root");
    Check(!sv::Map(L"C:\\Users\\player\\AppData\\Local\\KojimaProductionsX\\a", root, scratch, &out) &&
              !sv::Map(L"C:\\Users\\player\\AppData\\Local\\Temp\\a", root, scratch, &out) &&
              !sv::Map(L"C:\\Users\\player\\AppData\\Local", root, scratch, &out) && !sv::Map(L"", root, scratch, &out) &&
              !sv::Map(L"profile.dat", root, scratch, &out),
          "siblings, parents, other folders and relative names are not mapped");
    Check(sv::Map(L"C:\\Users\\player\\AppData\\Local\\Temp\\..\\KojimaProductions\\DeathStrandingDC\\1\\a", root, scratch, &out) &&
              out == scratch + L"\\DeathStrandingDC\\1\\a",
          "a '..' detour into the root is caught");
    Check(!sv::Map(scratch + L"\\DeathStrandingDC\\1\\a", root, scratch, &out), "mapping is idempotent (the scratch copy is not remapped)");
    Check(sv::Map(L"\\\\.\\C:\\Users\\player\\AppData\\Local\\KojimaProductions\\DeathStrandingDC\\1\\x", root, scratch, &out) &&
              out == L"\\\\?\\" + scratch + L"\\DeathStrandingDC\\1\\x",
          "a \\\\.\\ device path maps too", te::Narrow(out));
    Check(sv::Map(L"C:\\Users\\player\\AppData\\Local\\KojimaProductions.\\DeathStrandingDC\\1\\x", root, scratch, &out) &&
              out == scratch + L"\\DeathStrandingDC\\1\\x" &&
              sv::Map(L"C:\\Users\\player\\AppData\\Local\\KojimaProductions \\DeathStrandingDC. \\1\\x", root, scratch, &out) &&
              out == scratch + L"\\DeathStrandingDC\\1\\x",
          "trailing dots and spaces of a component (Win32 drops them) do not hide the root", te::Narrow(out));
    Check(sv::Map(root + L":stream", root, scratch, &out, &rel) && out == scratch + L":stream" &&
              sv::Map(root + L"\\a.dat:s", root, scratch, &out) && out == scratch + L"\\a.dat:s",
          "alternate data streams of the root and below map", te::Narrow(out));
    {
        const auto lookup = [](const std::wstring& p, std::wstring* name) {
            if (p == L"C:\\Users\\PLAYER~1") return *name = L"player", true;
            if (p == L"C:\\Users\\player\\AppData\\Local\\KOJIMA~1") return *name = L"KojimaProductions", true;
            return false;
        };
        const std::wstring exp = sv::ExpandShortNames(L"C:\\Users\\PLAYER~1\\AppData\\Local\\KOJIMA~1\\DeathStrandingDC\\x", lookup);
        Check(exp == root + L"\\DeathStrandingDC\\x" && sv::Map(exp, root, scratch, &out),
              "8.3 short names are expanded (component by component) before matching", te::Narrow(exp));
        Check(sv::ExpandShortNames(L"C:\\NOPE~1\\PLAYER~1\\x", lookup) == L"C:\\NOPE~1\\PLAYER~1\\x" &&
                  sv::ExpandShortNames(L"C:\\a\\b", lookup) == L"C:\\a\\b" && sv::ExpandShortNames(L"\\\\srv\\X~1\\a", lookup) == L"\\\\srv\\X~1\\a",
              "short-name expansion stops at a missing component and leaves other paths alone");
    }
    Check(sv::LogPrefix(L"DeathStrandingDC\\12345678\\autosave3\\checkpoint.dat") == L"DeathStrandingDC\\12345678\\autosave3" &&
              sv::LogPrefix(L"DeathStrandingDC\\12345678\\profile\\a\\b\\c.dat") == L"DeathStrandingDC\\12345678\\profile" &&
              sv::LogPrefix(L".st-redirect-probe").empty() && sv::LogPrefix(L"DeathStrandingDC\\x") == L"DeathStrandingDC",
          "log prefix: parent folder, at most 3 levels");
}

bool Put(const std::wstring& p, const std::string& s);
std::string Get(const std::wstring& p);

// The supervisor's save guard: backup, seed, compare, restore.
void SavesGuard(const std::wstring& work) {
    const std::wstring base = work + L"\\savesguard";
    te::RemoveTree(base);
    const std::wstring real = base + L"\\real\\KojimaProductions", run = base + L"\\run", scratch = base + L"\\game\\automation\\saves";
    const std::wstring dc = real + L"\\DeathStrandingDC\\1";
    te::EnsureDir(dc + L"\\profile");
    te::EnsureDir(dc + L"\\autosave9");  // empty folder
    te::EnsureDir(dc + L"\\quicksave0");
    te::EnsureDir(dc + L"\\autosave0");
    te::EnsureDir(run);
    Put(dc + L"\\profile\\profile.dat", "profile");
    Put(dc + L"\\autosave0\\checkpoint.dat", "a0");
    Put(dc + L"\\quicksave0\\checkpoint.dat", "q0");
    Put(dc + L"\\steam_autocloud.vdf", "vdf");
    std::string err;
    Check(te::BackupSaves(real, run, &err), "saves backed up (manifest + verified copy)", err);
    Check(!te::BackupSaves(real, run, &err), "a run folder backs the saves up once");
    std::string before;
    te::ReadFileBytes(run + L"\\saves-before.txt", &before);
    Check(before.find("DeathStrandingDC\\1\\autosave9\\\tdir") != std::string::npos &&
              before.find("DeathStrandingDC\\1\\profile\\profile.dat\t7\t") != std::string::npos,
          "the manifest lists folders (also empty ones) and files with size + sha256");
    Check(te::SeedScratch(run, scratch, &err) && Get(scratch + L"\\DeathStrandingDC\\1\\profile\\profile.dat") == "profile" &&
              te::DirExists(scratch + L"\\DeathStrandingDC\\1\\autosave9") && te::FileExists(scratch + L"\\.st-redirect-probe"),
          "scratch copy seeded (files, empty folders, the redirect probe)", err);
    Check(!te::FileExists(real + L"\\.st-redirect-probe"), "the probe exists only in the scratch copy");
    {  // --seed-saves: another save in the scratch copy only
        const std::wstring seed = base + L"\\seed\\DeathStrandingDC";
        te::EnsureDir(seed + L"\\manualsave4");
        te::EnsureDir(seed + L"\\profile");
        Put(seed + L"\\manualsave4\\checkpoint.dat", "seed-m4");
        Put(seed + L"\\profile\\profile.dat", "seed-profile");
        Put(seed + L"\\notes.txt", "ignored");
        int accounts = 0;
        Check(te::ApplySeedSaves(scratch, base + L"\\seed", &accounts, &err) && accounts == 1, "seed saves applied to the account folder", err);
        const std::wstring sa = scratch + L"\\DeathStrandingDC\\1";
        Check(Get(sa + L"\\manualsave4\\checkpoint.dat") == "seed-m4" && Get(sa + L"\\profile\\profile.dat") == "profile" &&
                  !te::DirExists(sa + L"\\autosave0") && !te::DirExists(sa + L"\\quicksave0") && Get(sa + L"\\steam_autocloud.vdf") == "vdf" &&
                  !te::FileExists(sa + L"\\notes.txt"),
              "seed: slots replaced; the user's profile and other account files kept; non-slot files not copied");
        Check(Get(dc + L"\\autosave0\\checkpoint.dat") == "a0" && Get(dc + L"\\profile\\profile.dat") == "profile",
              "seed: the real saves are untouched");
        Check(!te::ApplySeedSaves(scratch, base + L"\\no-seed", &accounts, &err), "seed: a folder without DeathStrandingDC is refused");
        Check(te::ManifestWithoutProbe(scratch).find(".st-redirect-probe") == std::string::npos, "seed: manifest without the probe");
        // --guest-seed profile-only: the guest's scratch keeps the profile and settings, no slot at all.
        Put(sa + L"\\bindings.cfg", "binds");
        int slots = 0;
        Check(te::StripScratchSlots(scratch, &slots, &err) && slots == 1 && !te::DirExists(sa + L"\\manualsave4") &&
                  Get(sa + L"\\profile\\profile.dat") == "profile" && Get(sa + L"\\bindings.cfg") == "binds" &&
                  Get(sa + L"\\steam_autocloud.vdf") == "vdf",
              "guest seed profile-only: every slot removed, profile and settings kept", err);
        Check(!te::StripScratchSlots(real, &slots, &err) && te::DirExists(dc + L"\\autosave0"),
              "guest seed profile-only: never a folder without the redirect probe (the real tree)");
        te::RemoveTree(scratch);
        Check(te::SeedScratch(run, scratch, &err), "scratch re-seeded from the backup after the seed test", err);
    }
    te::SavesCheck c = te::CheckAndRestoreSaves(run);
    Check(c.ran && c.unchanged && c.ok && Get(run + L"\\saves-restored.txt").rfind("ok\nunchanged", 0) == 0, "untouched saves: unchanged");
    // What a run without the redirect could do:
    Put(dc + L"\\profile\\profile.dat", "changed");
    DeleteFileW((dc + L"\\quicksave0\\checkpoint.dat").c_str());
    RemoveDirectoryW((dc + L"\\quicksave0").c_str());
    te::EnsureDir(dc + L"\\autosave1");
    Put(dc + L"\\autosave1\\checkpoint.dat", "new");
    te::EnsureDir(dc + L"\\autosave2");
    RemoveDirectoryW((dc + L"\\autosave9").c_str());
    c = te::CheckAndRestoreSaves(run);
    std::string notes;
    for (const auto& n : c.notes) notes += n + "; ";
    Check(c.ran && !c.unchanged && c.changes.size() == 7 && c.ok, "changed saves are detected and restored", notes);
    Check(Get(dc + L"\\profile\\profile.dat") == "profile" && Get(dc + L"\\quicksave0\\checkpoint.dat") == "q0" &&
              te::DirExists(dc + L"\\autosave9") && !te::DirExists(dc + L"\\autosave1") && !te::DirExists(dc + L"\\autosave2"),
          "changed/removed files and folders back, new ones gone from the save tree");
    Check(Get(run + L"\\saves-added\\DeathStrandingDC\\1\\autosave1\\checkpoint.dat") == "new", "a new file is moved to saves-added, not deleted");
    Check(te::DiffManifests(before, te::HashManifest(real)).empty() && Get(run + L"\\saves-restored.txt").rfind("ok\nrestored", 0) == 0,
          "after the restore the tree matches the manifest again");
    c = te::CheckAndRestoreSaves(run);
    Check(c.unchanged && c.ok, "a second check finds nothing to do");
    te::SavesCheck none = te::CheckAndRestoreSaves(base + L"\\no-such-run");
    Check(!none.ran && none.ok, "no saves-restore.txt: nothing to check");
    // Steam's cloud log
    const std::string cloud =
        "[2026-11-16 11:32:05] [AppID 1850570]\tSkipping un-modified file KojimaProductions/DeathStrandingDC/1/profile/profile.dat\r\n"
        "[2026-11-16 11:32:05] [AppID 1850570] AutoCloud complete\n"
        "[2026-11-16 11:32:06] [AppID 999] Need to upload file x\n"
        "[2026-11-16 08:04:40] [AppID 1850570] Need to upload file KojimaProductions/DeathStrandingDC/1/profile/profile.dat\n"
        "[2026-11-16 08:04:41] [AppID 1850570] Upload OK for file KojimaProductions/DeathStrandingDC/1/profile/profile.dat\n";
    const auto lines = te::CloudLogLines(cloud, "1850570");
    const auto ups = te::CloudUploads(lines);
    Check(lines.size() == 4 && ups.size() == 2 && lines[0].back() != '\r', "Steam cloud_log: app lines and upload lines");
    te::RemoveTree(base);
}

bool Put(const std::wstring& p, const std::string& s) { return te::WriteFileAtomic(p, s); }
std::string Get(const std::wstring& p) {
    std::string s;
    return te::ReadFileBytes(p, &s) ? s : std::string("<missing>");
}

void BackupRestore(const std::wstring& work) {
    const std::wstring root = work + L"\\guard";
    te::RemoveTree(root);
    const std::wstring game = root + L"\\game", mod = game + L"\\stranded_together", run = root + L"\\run";
    te::EnsureDir(mod);
    te::EnsureDir(run);
    Put(game + L"\\ds.exe", "exe");
    Put(game + L"\\winhttp.dll", "original dll");
    Put(mod + L"\\session.ini", "role=echo\n");
    Put(mod + L"\\positions.txt", "1 2 3 home\n");
    Put(mod + L"\\stranded_together.log", "old log\n");
    std::string err;
    Check(te::BackupGameFolder(game, run, &err), "backup of a game folder", err);
    Check(!te::BackupGameFolder(game, run, &err), "a run folder is used once (restore.txt kept)");
    // What a hidden run does to the folder:
    Put(game + L"\\winhttp.dll", "our dll");
    Put(mod + L"\\session.ini", "automation=1\n");
    MoveFileExW((mod + L"\\session.ini").c_str(), (mod + L"\\session.last.ini").c_str(), MOVEFILE_REPLACE_EXISTING);
    Put(mod + L"\\stranded_together.log", "new log\n");
    Put(mod + L"\\link-status.json", "{}");
    Put(mod + L"\\something-new.txt", "new");
    Put(game + L"\\error.txt", "crash");
    te::EnsureDir(mod + L"\\automation\\out");
    Put(mod + L"\\automation\\out\\a.png", "png");
    const te::GuardResult r = te::RestoreGameFolder(run);
    std::string notes;
    for (const auto& n : r.notes) notes += n + "; ";
    Check(r.ok, "restore reports success", notes);
    Check(Get(game + L"\\winhttp.dll") == "original dll", "winhttp.dll restored");
    Check(Get(mod + L"\\session.ini") == "role=echo\n", "session.ini restored");
    Check(!te::FileExists(mod + L"\\session.last.ini") && !te::FileExists(mod + L"\\link-status.json"),
          "files that did not exist before are removed");
    Check(Get(mod + L"\\stranded_together.log") == "old log\n" && Get(mod + L"\\positions.txt") == "1 2 3 home\n",
          "log restored, untouched files kept");
    Check(!te::FileExists(game + L"\\error.txt"), "a new error.txt is removed (the supervisor copies it first)");
    Check(!te::DirExists(mod + L"\\automation"), "the automation folder is removed");
    Check(Get(run + L"\\game-new\\something-new.txt") == "new" && !te::FileExists(mod + L"\\something-new.txt"),
          "a new file in stranded_together is moved to <run>\\game-new");
    Check(te::FileExists(run + L"\\restored.txt"), "restored.txt written");
    const te::GuardResult again = te::RestoreGameFolder(run);
    Check(again.ok && Get(game + L"\\winhttp.dll") == "original dll", "restore is idempotent");

    // A folder without stranded_together and without winhttp.dll.
    const std::wstring game2 = root + L"\\game2", run2 = root + L"\\run2";
    te::EnsureDir(game2);
    te::EnsureDir(run2);
    Put(game2 + L"\\ds.exe", "exe");
    err.clear();
    Check(te::BackupGameFolder(game2, run2, &err), "backup of a clean folder", err);
    te::EnsureDir(game2 + L"\\stranded_together\\automation\\in");
    Put(game2 + L"\\winhttp.dll", "our dll");
    Put(game2 + L"\\stranded_together\\session.last.ini", "x");
    Put(game2 + L"\\stranded_together\\stranded_together.log", "x");
    const te::GuardResult r2 = te::RestoreGameFolder(run2);
    Check(r2.ok && !te::FileExists(game2 + L"\\winhttp.dll") && !te::DirExists(game2 + L"\\stranded_together") &&
              te::FileExists(game2 + L"\\ds.exe"),
          "clean folder: our dll and our stranded_together folder removed, ds.exe kept");

    // A tampered manifest never reaches outside the game folder.
    const std::wstring outside = root + L"\\outside.txt", run3 = root + L"\\run3";
    Put(outside, "keep me");
    te::EnsureDir(run3);
    Put(run3 + L"\\restore.txt", "game\t" + te::Narrow(game) + "\nfile\t0\t" + te::Narrow(outside) + "\t-\t-\ntree\t" + te::Narrow(root) + "\n");
    te::RestoreGameFolder(run3);
    Check(Get(outside) == "keep me" && te::DirExists(game), "restore ignores paths outside the game folder");

    // Save manifests
    const std::wstring saves = root + L"\\saves";
    te::EnsureDir(saves + L"\\autosave0");
    Put(saves + L"\\autosave0\\checkpoint.dat", "a");
    Put(saves + L"\\profile.dat", "p");
    const std::string m1 = te::HashManifest(saves);
    Put(saves + L"\\profile.dat", "p2");
    Put(saves + L"\\autosave1.dat", "n");
    DeleteFileW((saves + L"\\autosave0\\checkpoint.dat").c_str());
    const auto diff = te::DiffManifests(m1, te::HashManifest(saves));
    std::string d;
    for (const auto& x : diff) d += x + "; ";
    Check(diff.size() == 3 && d.find("changed profile.dat") != std::string::npos && d.find("added autosave1.dat") != std::string::npos &&
              d.find("removed autosave0\\checkpoint.dat") != std::string::npos,
          "save manifest diff: changed / added / removed", d);
    te::RemoveTree(root);
}

// ------------------------------------------------------------------ e2e
bool DecodePng(const std::wstring& path, std::vector<unsigned char>& bgra, UINT& w, UINT& h) {
    IWICImagingFactory* f = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapSource* conv = nullptr;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) &&
              SUCCEEDED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
              SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(WICConvertBitmapSource(GUID_WICPixelFormat32bppBGRA, frame, &conv)) &&
              SUCCEEDED(conv->GetSize(&w, &h));
    if (ok) {
        bgra.resize(static_cast<std::size_t>(w) * h * 4);
        ok = SUCCEEDED(conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(bgra.size()), bgra.data()));
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    if (f) f->Release();
    return ok;
}

bool CenterIs(const std::wstring& png, int r, int g, int b, std::string* got) {
    std::vector<unsigned char> px;
    UINT w = 0, h = 0;
    if (!DecodePng(png, px, w, h) || !w || !h) {
        *got = "not decodable";
        return false;
    }
    const unsigned char* p = &px[(static_cast<std::size_t>(h / 2) * w + w / 2) * 4];
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%ux%u centre (%d,%d,%d) want (%d,%d,%d)", w, h, p[2], p[1], p[0], r, g, b);
    *got = buf;
    return std::abs(p[2] - r) <= 2 && std::abs(p[1] - g) <= 2 && std::abs(p[0] - b) <= 2;
}

// automation=1 left behind (an interrupted run) and the game started by something else
// than the supervisor (Steam): the mod must end it in DllMain, before it runs. The fake
// game is started on a private desktop in a kill-on-close job, never on the user's.
void RefusedOutsideSupervisor(const std::wstring& game, const std::wstring& mod) {
    const std::wstring mod_dir = game + L"\\stranded_together";
    Check(CopyFileW(mod.c_str(), (game + L"\\winhttp.dll").c_str(), FALSE) != FALSE, "refusal: mod deployed into the fake game folder");
    te::EnsureDir(mod_dir + L"\\automation\\saves");
    Put(mod_dir + L"\\automation\\saves\\.st-redirect-probe", "probe");  // a seeded scratch copy is there too
    // A stale --seed-saves session.ini (interrupted run): refused before the foreign-saves bypass too.
    Put(mod_dir + L"\\session.ini", "one_shot=1\nautomation=1\nautomation_foreign_saves=1\n");
    SetEnvironmentVariableW(au::saves::kSupervisedEnv, nullptr);
    wchar_t name[64];
    std::swprintf(name, 64, L"StTestEnvRefusal_%lu", GetCurrentProcessId());
    HDESK desk = CreateDesktopW(name, nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim{};
    lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    const bool setup = desk && job && SetInformationJobObject(job, JobObjectExtendedLimitInformation, &lim, sizeof(lim));
    Check(setup, "refusal: private desktop and job");
    DWORD code = 0;
    if (setup) {
        std::wstring desktop = std::wstring(L"WinSta0\\") + name;
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.lpDesktop = desktop.data();
        PROCESS_INFORMATION pi{};
        std::wstring exe = game + L"\\ds.exe", cmd = L"\"" + exe + L"\"";
        if (CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, game.c_str(), &si, &pi)) {
            if (AssignProcessToJobObject(job, pi.hProcess)) {
                ResumeThread(pi.hThread);
                if (WaitForSingleObject(pi.hProcess, 15000) != WAIT_OBJECT_0) TerminateJobObject(job, 1);
                WaitForSingleObject(pi.hProcess, 5000);
                GetExitCodeProcess(pi.hProcess, &code);
            } else {
                TerminateProcess(pi.hProcess, 1);
            }
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }
    if (job) CloseHandle(job);
    if (desk) CloseDesktop(desk);
    const std::string refused = Get(mod_dir + L"\\automation\\" + au::saves::kRefusedFile);
    char hex[32];
    std::snprintf(hex, sizeof(hex), "0x%08lX", code);
    Check(code == au::saves::kRefusedExitCode && refused.find("not started by st_testenv_supervisor") != std::string::npos,
          "automation=1 in a ds.exe the supervisor did not start is refused in DllMain", std::string(hex) + " " + refused);
    Check(!te::FileExists(mod_dir + L"\\fakegame.log"), "refusal: the fake game's own code never ran");
    Check(!te::FileExists(mod_dir + L"\\automation\\" + au::saves::kActiveFile),
          "refusal: no redirect and no foreign-saves bypass (stale automation_foreign_saves=1 ignored)");
    te::RemoveTree(mod_dir + L"\\automation");
    DeleteFileW((mod_dir + L"\\session.ini").c_str());
    Put(game + L"\\winhttp.dll", "original winhttp.dll (not a DLL)");
}

int EndToEnd(const std::wstring& supervisor, const std::wstring& fakegame, const std::wstring& mod, const std::wstring& scenario,
             const std::wstring& work_in) {
    if (!te::FindProcesses(L"ds.exe").empty()) {
        std::printf("SKIP  a ds.exe is running: the supervisor refuses to start (by design)\n");
        return 77;
    }
    const std::wstring work = te::FullPath(work_in) + L"\\e2e";
    te::RemoveTree(work);
    const std::wstring game = work + L"\\game", run = work + L"\\run";
    te::EnsureDir(game + L"\\stranded_together");
    Check(CopyFileW(fakegame.c_str(), (game + L"\\ds.exe").c_str(), FALSE) != FALSE, "fake game copied as ds.exe");
    Put(game + L"\\winhttp.dll", "original winhttp.dll (not a DLL)");
    Put(game + L"\\stranded_together\\session.ini", "# original session\nrole=echo\n");
    // A scratch "real" save root: the fake game uses it at start-up like ds.exe's user:
    // device; the mod must redirect all of it into the seeded scratch copy.
    const std::wstring saves = work + L"\\realsaves\\KojimaProductions";
    te::EnsureDir(saves + L"\\DeathStrandingDC\\1\\profile");
    te::EnsureDir(saves + L"\\DeathStrandingDC\\1\\quicksave0");
    Put(saves + L"\\DeathStrandingDC\\1\\profile\\profile.dat", "seed-profile");
    Put(saves + L"\\DeathStrandingDC\\1\\quicksave0\\checkpoint.dat", "q0");
    const std::string saves_before = te::HashManifest(saves);
    // --seed-saves: the supervisor seeds this slot and asks for the foreign-saves bypass; on the
    // fake game the byte check must refuse (no ds.exe code there) and the run go on unpatched.
    const std::wstring seed = work + L"\\seed";
    te::EnsureDir(seed + L"\\DeathStrandingDC\\quicksave0");
    Put(seed + L"\\DeathStrandingDC\\quicksave0\\checkpoint.dat", "seed-q0");
    SetEnvironmentVariableW(L"ST_FAKEGAME_SAVE_ROOT", saves.c_str());  // inherited by the supervisor and the fake game
    std::wstring cmd = L"\"" + supervisor + L"\" --game-dir \"" + game + L"\" --mod \"" + mod + L"\" --scenario \"" + scenario +
                       L"\" --run-dir \"" + run + L"\" --saves \"" + saves + L"\" --seed-saves \"" + seed +
                       L"\" --no-steam-check --timeout-s 150";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const ULONGLONG t0 = GetTickCount64();
    if (!CreateProcessW(supervisor.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, work.c_str(), &si, &pi)) {
        Check(false, "supervisor started", std::to_string(GetLastError()));
        return 1;
    }
    const DWORD waited = WaitForSingleObject(pi.hProcess, 240000);
    DWORD code = 99;
    if (waited != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 99);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    std::printf("info  supervisor exit %lu after %.1f s; run folder %s\n", code, static_cast<double>(GetTickCount64() - t0) / 1000.0,
                te::Narrow(run).c_str());
    if (code == 2) {  // refused: a game or another hidden run (e.g. another build folder's ctest) is active
        const std::string log = Get(run + L"\\supervisor.log");
        if (log.find("is already running") != std::string::npos || log.find("another st_testenv_supervisor") != std::string::npos) {
            std::printf("SKIP  the supervisor refused to start (another run or a ds.exe is active)\n");
            return 77;
        }
    }
    std::string summary = Get(run + L"\\summary.json");
    Check(code == 0 && summary.find("\"result\":\"pass\"") != std::string::npos, "supervisor run passed",
          summary.substr(0, 300));
    Check(summary.find("\"violation\":false") != std::string::npos, "no window on the user's desktop, input desktop unchanged");
    std::string got;
    Check(CenterIs(run + L"\\automation\\fake-01-before.png", 26, 77, 102, &got), "screenshot before A: the fake game's colour 0", got);
    Check(CenterIs(run + L"\\automation\\fake-02-after-a.png", 204, 51, 26, &got), "screenshot after virtual A: colour 1", got);
    std::vector<unsigned char> px;
    UINT w = 0, h = 0;
    Check(DecodePng(run + L"\\automation\\fake-03-menu.png", px, w, h) && w == 960 && h == 540, "screenshot with the debug menu open");
    const std::string fake = Get(run + L"\\game-new\\fakegame.log");
    Check(fake.find("A pressed") != std::string::npos, "the virtual pad reached the game's own XInputGetState call");
    Check(fake.find("self-deactivation before the first frame: app active=0") != std::string::npos &&
              fake.find("WM_ACTIVATEAPP wParam=1: app active=1") != std::string::npos,
          "the mod posted WM_ACTIVATEAPP(TRUE) to the hidden window (ds.exe ignores pads while not active)");
    Check(fake.find("self-deactivation at frame 60: app active=1") != std::string::npos,
          "a later WM_ACTIVATEAPP(FALSE) reached the game as TRUE");
    Check(fake.find("left stick up (ly=32767") != std::string::npos, "the virtual pad's left stick reached the game");
    Check(fake.find("XInputSetState(0, motors off) -> 0") != std::string::npos, "XInputSetState for the virtual pad answers success");
    Check(fake.find("GetForegroundWindow()==window: yes") != std::string::npos, "GetForegroundWindow answers the hidden game window");
    Check(fake.find("WM_CLOSE: exiting") != std::string::npos, "quit closed the game through WM_CLOSE");
    const std::string modlog = Get(run + L"\\game\\stranded_together.log");
    Check(modlog.find("[auto] channel ready") != std::string::npos && modlog.find("AUTOMATION") != std::string::npos,
          "mod log: automation channel and overlay automation mode");
    Check(modlog.find("XInputSetState(0) vibration 0/0 kept from the real pads") != std::string::npos,
          "the game's XInputSetState was answered by the mod (rumble never reaches a real pad)");
    Check(Get(game + L"\\winhttp.dll") == "original winhttp.dll (not a DLL)", "winhttp.dll restored after the run");
    Check(Get(game + L"\\stranded_together\\session.ini") == "# original session\nrole=echo\n" &&
              !te::FileExists(game + L"\\stranded_together\\session.last.ini") && !te::DirExists(game + L"\\stranded_together\\automation"),
          "session.ini restored, session.last.ini and automation removed");
    Check(te::FindProcesses(L"ds.exe").empty(), "no ds.exe left running");
    // saves
    Check(summary.find("\"saves_redirected\":true") != std::string::npos && summary.find("\"saves_unchanged\":true") != std::string::npos,
          "summary: saves redirected (confirmed before the steps) and unchanged");
    Check(modlog.find("[auto] saves=redirected") != std::string::npos &&
              modlog.find("[auto] saves: write DeathStrandingDC\\1\\autosave0 -> scratch") != std::string::npos,
          "mod log: saves=redirected and one line per redirected place");
    Check(fake.find("save read profile.dat: 'seed-profile'") != std::string::npos &&
              fake.find("save read with '/' separators: 'seed-profile'") != std::string::npos,
          "the game read the seeded profile through the redirect");
    Check(fake.find("save exercise: 12/12 ok") != std::string::npos, "every save operation of the fake game succeeded (in the scratch copy)");
    {
        const std::wstring a0 = run + L"\\saves-scratch-changed\\DeathStrandingDC\\1\\autosave0\\";
        Check(Get(a0 + L"device.dat") == "d" && Get(a0 + L"trailing-dot.dat") == "t" && Get(a0 + L"relative.dat") == "r" &&
                  Get(a0 + L"short.dat") == "s",
              "\\\\.\\, trailing-dot, relative and 8.3 spellings of the save folder were redirected too");
    }
    Check(te::HashManifest(saves) == saves_before && !te::FileExists(saves + L"\\.st-redirect-probe"),
          "the 'real' save tree is byte for byte unchanged");
    Check(Get(run + L"\\saves-scratch-changed\\DeathStrandingDC\\1\\autosave0\\checkpoint.dat") == "fake checkpoint" &&
              Get(run + L"\\saves-scratch-changed\\DeathStrandingDC\\1\\autosave1\\checkpoint.dat") == "fake checkpoint" &&
              Get(run + L"\\saves-scratch-changed\\DeathStrandingDC\\1\\manual.dat") == "m",
          "the writes landed in the scratch copy (collected to saves-scratch-changed)");
    Check(summary.find("removed DeathStrandingDC\\\\1\\\\quicksave0\\\\checkpoint.dat") != std::string::npos,
          "the delete hit the scratch copy (scratch_changes)");
    Check(Get(run + L"\\game\\saves-redirect.txt").rfind("saves=redirected", 0) == 0, "DllMain marker collected");
    Check(summary.find("\"foreign_saves\":true") != std::string::npos &&
              summary.find("[auto] foreign saves: NOT bypassed (nothing patched)") != std::string::npos &&
              modlog.find("check bypassed") == std::string::npos &&
              Get(run + L"\\game\\saves-redirect.txt").find("foreign_saves=refused\n") != std::string::npos,
          "--seed-saves: automation_foreign_saves=1 sent; the byte check refused on a non-ds.exe image (nothing patched)");
    Check(Get(game + L"\\stranded_together\\session.ini").find("foreign") == std::string::npos, "restored session.ini has no foreign-saves key");
    RefusedOutsideSupervisor(game, mod);
    return g_fail ? 1 : 0;
}

}  // namespace

// Test saves and two games on one PC (native/mod/sandbox.hpp, docs/sandbox-and-local-test.md).
void SandboxRules() {
    namespace sb = st::sandbox;
    Check(sb::ValidSandboxName("test") && sb::ValidSandboxName("local-guest") && sb::ValidSandboxName("testenv-1234") &&
              !sb::ValidSandboxName("") && !sb::ValidSandboxName("..") && !sb::ValidSandboxName("a\\b") && !sb::ValidSandboxName("a/b") &&
              !sb::ValidSandboxName(std::string(41, 'a')),
          "sandbox names: letters, digits, '_' and '-' only (never a path)");
    std::wstring rel;
    Check(sb::ValidInstanceDir(L"instances\\guest", &rel) && rel == L"instances\\guest" && sb::ValidInstanceDir(L"automation/guest", &rel) &&
              rel == L"automation\\guest" && !sb::ValidInstanceDir(L"..\\x") && !sb::ValidInstanceDir(L"\\abs") && !sb::ValidInstanceDir(L"C:\\x") &&
              !sb::ValidInstanceDir(L"a\\\\b") && !sb::ValidInstanceDir(L"a\\b\\c\\d") && !sb::ValidInstanceDir(L""),
          "ST_INSTANCE_DIR: 1..3 plain components below stranded_together");
    bool used = true;
    Check(sb::ModDirFor(L"C:\\G", L"", &used) == L"C:\\G\\stranded_together" && !used &&
              sb::ModDirFor(L"C:\\G", L"instances\\host", &used) == L"C:\\G\\stranded_together\\instances\\host" && used &&
              sb::ModDirFor(L"C:\\G", L"..\\..\\Windows", &used) == L"C:\\G\\stranded_together" && !used,
          "the instance mod folder (an invalid ST_INSTANCE_DIR falls back to the default)");
    Check(sb::RenamedMutex("DeathStrandingDCAppMutex") == "DeathStrandingDCAppMutex-StrandedTogether-2" &&
              sb::RenamedMutex("Local\\DeathStrandingDCAppMutex") == "Local\\DeathStrandingDCAppMutex-StrandedTogether-2" &&
              sb::RenamedMutex("DeathStrandingDCAppMutex2").empty() && sb::RenamedMutex("Other").empty() && sb::RenamedMutex(nullptr).empty() &&
              sb::RenamedMutexW(L"DeathStrandingDCAppMutex") == L"DeathStrandingDCAppMutex-StrandedTogether-2",
          "second instance: only ds.exe's single-instance mutex is renamed");
    sb::Request r;
    Check(sb::Decide(r) == sb::Mode::none, "no keys: nothing installed (every normal launch)");
    r.sandbox = "test";
    Check(sb::Decide(r) == sb::Mode::sandbox, "sandbox_saves=test: the visible-play redirect");
    r.automation = true;
    Check(sb::Decide(r) == sb::Mode::automation, "automation=1 wins (hidden runs keep their own rules)");
    r = sb::Request{};
    r.sandbox = "..\\real";
    Check(sb::Decide(r) == sb::Mode::refuse_bad_name, "a sandbox name that is a path: refused");
    for (int k = 0; k < 3; ++k) {
        r = sb::Request{};
        (k == 0 ? r.second_instance : k == 1 ? r.appdir_settings : r.sandbox_foreign) = true;
        Check(sb::Decide(r) == sb::Mode::refuse_orphan, "second_instance / appdir_settings / sandbox_foreign_saves without a redirect: refused");
    }
    r = sb::Request{};
    r.instance_env = true;
    Check(sb::Decide(r) == sb::Mode::refuse_instance, "ST_INSTANCE_DIR without a test save / hidden run: refused (never the real saves)");
    r.sandbox = "local-guest";
    Check(sb::Decide(r) == sb::Mode::sandbox, "ST_INSTANCE_DIR with sandbox_saves: the local test's redirect");
    r = sb::Request{};
    r.instance_env = true;
    r.automation = true;
    Check(sb::Decide(r) == sb::Mode::automation, "ST_INSTANCE_DIR with automation=1: a hidden two-game run's guest");
    // The host-held save's guest (role=guest host_save=1): only on a redirect, never on the real saves.
    r = sb::Request{};
    r.guest_host_save = true;
    Check(sb::Decide(r) == sb::Mode::refuse_host_save, "role=guest host_save=1 without a redirect: refused (exit 0x53540006)");
    r.sandbox = sb::kCoopGuestSandbox;
    Check(sb::Decide(r) == sb::Mode::sandbox, "role=guest host_save=1 with sandbox_saves=coop-guest: the redirect");
    r.sandbox.clear();
    r.automation = true;
    Check(sb::Decide(r) == sb::Mode::automation, "role=guest host_save=1 in a hidden run: the automation redirect");
    r = sb::Request{};
    r.guest_host_save = true;
    r.sandbox = "..\\x";
    Check(sb::Decide(r) == sb::Mode::refuse_bad_name, "role=guest host_save=1 with a bad sandbox name: refused");
    r = sb::Request{};
    r.guest_host_save = true;
    r.instance_env = true;
    r.second_instance = true;
    Check(sb::Decide(r) == sb::Mode::refuse_host_save, "a host-save guest's refusal comes first (one reason)");
    Check(sb::SandboxForeignAllowed(true, true) && !sb::SandboxForeignAllowed(true, false) && !sb::SandboxForeignAllowed(false, true),
          "test-save foreign-save bypass: only requested and after the redirect self-test");
    Check(sb::ParseLocalInput("host") == sb::LocalInput::host && sb::ParseLocalInput("guest") == sb::LocalInput::guest &&
              sb::ParseLocalInput("host_pad") == sb::LocalInput::host_pad && sb::ParseLocalInput("x") == sb::LocalInput::off,
          "local_input values");
    const std::string merged = sb::MergeGameSettings("\"GameDifficulty\" \"4\"\r\n\"WindowMode\" \"0\"\r\n", "\"WindowMode\" \"1\"\r\n\"GraphicQuality\" \"0\"\r\n");
    Check(merged == "\"GameDifficulty\" \"4\"\r\n\"WindowMode\" \"1\"\r\n\"GraphicQuality\" \"0\"\r\n", "game_settings.cfg merge: replace + append, CRLF kept", merged);
    {
        const std::string low = sb::LowGraphicsSettingsCfg();
        bool lines_ok = !low.empty();
        std::size_t at = 0;
        while (lines_ok && at < low.size()) {
            const std::size_t nl = low.find("\r\n", at);
            const std::string line = low.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
            lines_ok = std::count(line.begin(), line.end(), '"') == 4 && line.front() == '"' && line.back() == '"';
            at = nl == std::string::npos ? low.size() : nl + 2;
        }
        Check(lines_ok, "the low graphics settings.cfg text is \"key\" \"value\" lines (the game's own format)");
        // "graphics low <fps>" replaces these two lines (supervisor.cpp)
        Check(low.find("\"fps\" \"30\"") != std::string::npos && low.find("\"limit_fps\" \"30\"") != std::string::npos,
              "the low preset carries the fps / limit_fps lines graphics low <fps> replaces");
        te::Scenario g;
        std::string gerr;
        Check(te::ParseScenario("role host\ngraphics low 120\nquit\n", &g, &gerr) && g.low_graphics && g.low_graphics_fps == 120, "graphics low 120", gerr);
        Check(te::ParseScenario("role host\ngraphics low\nquit\n", &g, &gerr) && g.low_graphics_fps == 30, "graphics low: 30 fps", gerr);
        Check(!te::ParseScenario("role host\ngraphics low 20\nquit\n", &g, &gerr) && !te::ParseScenario("role host\ngraphics low 12x\nquit\n", &g, &gerr),
              "graphics low <fps>: 30..240 digits only");
    }

    // the two-game session.ini files
    te::SessionOptions o;
    o.role = "host";
    o.appdir_settings = true;
    o.frame_cap = 30;
    o.local_window = "left";
    const std::string host = te::SessionIniText(o);
    o.role = "guest";
    o.port_local = 27556;
    o.port_peer = 27555;
    o.second_instance = true;
    o.local_window = "right";
    const std::string guest = te::SessionIniText(o);
    Check(host.find("role=host\n") != std::string::npos && host.find("udp_port_local=27555\n") != std::string::npos &&
              host.find("second_instance") == std::string::npos && host.find("frame_cap=30\n") != std::string::npos &&
              host.find("appdir_settings=1\n") != std::string::npos && host.find("local_window=left\n") != std::string::npos,
          "two-game run: the host's session.ini");
    Check(guest.find("role=guest\n") != std::string::npos && guest.find("udp_port_local=27556\nudp_port_peer=27555\n") != std::string::npos &&
              guest.find("second_instance=1\n") != std::string::npos && guest.find("automation=1\n") != std::string::npos,
          "two-game run: the guest's session.ini");
    Check(host.rfind("# Generated by Stranded Together", 0) != 0, "testenv files never carry the launcher's header (an open launcher would delete them)");
    const std::string sbs = te::SandboxSessionIniText("testenv-1", true);
    Check(sbs.find("sandbox_saves=testenv-1\n") != std::string::npos && sbs.find("sandbox_foreign_saves=1\n") != std::string::npos &&
              sbs.find("automation") == std::string::npos,
          "--sandbox-session: the launcher-style test-save session.ini (no automation=1)");

    // scenarios: @guest / @both need role host
    te::Scenario sc;
    std::string err;
    Check(te::ParseScenario("role host\ngraphics low\n@guest pad A 100\n@both screenshot x\n?@guest waitlog 5 abc\nquit\n", &sc, &err) &&
              sc.steps.size() == 4 && sc.steps[0].target == 1 && sc.steps[1].target == 2 && sc.steps[2].target == 1 && sc.steps[2].optional &&
              sc.steps[3].target == 0 && sc.low_graphics,
          "scenario: @guest / @both steps", err);
    Check(!te::ParseScenario("role solo\n@guest pad A 100\n", &sc, &err), "scenario: @guest without role host is refused");
    Check(!te::ParseScenario("role host\n@both waitlog 5 x\n", &sc, &err), "scenario: @both only for mod commands and waitready");
    Check(!te::ParseScenario("role host\n@guest @guest quit\n", &sc, &err), "scenario: one @ prefix per step");
    // guest unredirected (guest-refuse-unredirected): role host only, no @guest step / ini, the guest's file has no redirect
    Check(te::ParseScenario("role host\nguest unredirected\nini host_save=1\nwaitready 5\nquit\n", &sc, &err) && sc.guest_unredirected,
          "scenario: guest unredirected", err);
    Check(!te::ParseScenario("role solo\nguest unredirected\nquit\n", &sc, &err), "scenario: guest unredirected needs role host");
    Check(!te::ParseScenario("role host\nguest unredirected\n@guest pad A 100\nquit\n", &sc, &err), "scenario: guest unredirected refuses @guest steps");
    Check(!te::ParseScenario("role host\nguest unredirected\n@guest ini name=x\nquit\n", &sc, &err), "scenario: guest unredirected refuses @guest ini");
    Check(!te::ParseScenario("role host\n?guest unredirected\nquit\n", &sc, &err), "scenario: guest unredirected cannot be optional");
    Check(te::ScenarioIniKeyAllowed("host_save") && te::ScenarioIniKeyAllowed("coop_test_build") && !te::ScenarioIniKeyAllowed("sandbox_saves"),
          "scenario ini: host_save and coop_test_build allowed, sandbox_saves not");
    Check(te::ScenarioIniKeyAllowed("coop_test_title_off") && te::ScenarioIniKeyAllowed("guest_skip_logos"),
          "scenario ini: coop_test_title_off and guest_skip_logos allowed");
    Check(te::ScenarioIniKeyAllowed("test_net_latency_ms") && te::ScenarioIniKeyAllowed("test_net_burst"), "scenario ini: the test_net_* impairment allowed");
    // launcherini (the launcher's own Local test files): role host, graphics low, no ini lines
    Check(te::ParseScenario("role host\ngraphics low\nlauncherini local-test\nwaitready 5\nquit\n", &sc, &err) && sc.launcher_ini == "local-test",
          "scenario: launcherini local-test", err);
    Check(!te::ParseScenario("role solo\ngraphics low\nlauncherini local-test\nquit\n", &sc, &err), "scenario: launcherini needs role host");
    Check(!te::ParseScenario("role host\ngraphics low\nlauncherini local-test\nini host_save=1\nquit\n", &sc, &err),
          "scenario: launcherini refuses ini lines (the launcher's file exactly)");
    Check(!te::ParseScenario("role host\ngraphics low\nlauncherini local-test\n@guest ini partner_collision=0\nquit\n", &sc, &err),
          "scenario: launcherini refuses @guest ini");
    Check(!te::ParseScenario("role host\nlauncherini local-test\nquit\n", &sc, &err), "scenario: launcherini needs graphics low");
    Check(!te::ParseScenario("role host\ngraphics low\nlauncherini online\nquit\n", &sc, &err), "scenario: launcherini local-test only");
    {
        const std::string launcher =
            "# Generated by Stranded Together. The mod renames it to session.last.ini once read; without this file the game runs unmodded.\n"
            "role=guest\ntransport=udp\nsecret=K7RD3WQ9MP2XVB6TZHN4\nudp_peer=127.0.0.1\nudp_port_local=27556\nudp_port_peer=27555\n"
            "verbose=0\ndebug_menu=1\none_shot=1\nsandbox_saves=local-guest\nsandbox_foreign_saves=1\nappdir_settings=1\nsecond_instance=1\n"
            "host_save=1\nlocal_input=guest\nframe_cap=30\nlocal_window=right\nmy_character=fragile_suit\npartner_body=real\nself_character=1\n";
        te::SessionOptions go;
        go.role = "guest";
        go.foreign_saves = true;
        std::string out, lerr;
        std::vector<std::string> swapped;
        Check(te::HiddenIniFromLauncher(launcher, go, &out, &swapped, &lerr) && swapped.size() == 2, "launcherini: the launcher's guest file", lerr);
        Check(out.rfind("# Generated by Stranded Together", 0) != 0 && out.find("sandbox_saves") == std::string::npos &&
                  out.find("sandbox_foreign_saves") == std::string::npos && out.find("automation=1\n") != std::string::npos &&
                  out.find("automation_foreign_saves=1\n") != std::string::npos && out.find("automation_mute=1\n") != std::string::npos,
              "launcherini: the save location swapped for the run's redirect, the launcher's header gone");
        for (const char* kept : {"role=guest\n", "host_save=1\n", "second_instance=1\n", "appdir_settings=1\n", "local_input=guest\n",
                                 "frame_cap=30\n", "my_character=fragile_suit\n", "self_character=1\n", "verbose=0\n", "secret=K7RD3WQ9MP2XVB6TZHN4\n"})
            Check(out.find(kept) != std::string::npos, "launcherini: the launcher's key is kept", kept);
        go.foreign_saves = false;
        Check(te::HiddenIniFromLauncher(launcher, go, &out, &swapped, &lerr) && out.find("automation_foreign_saves") == std::string::npos,
              "launcherini: automation_foreign_saves only with a seed");
        Check(!te::HiddenIniFromLauncher("role=guest\nhost_save=1\n", go, &out, &swapped, &lerr), "launcherini: not the launcher's file: refused");
        Check(!te::HiddenIniFromLauncher(launcher + "automation=1\n", go, &out, &swapped, &lerr), "launcherini: a file with automation keys: refused");
    }
    const std::string ug = te::UnredirectedGuestIniText(27556, 27555);
    Check(ug.find("role=guest\n") != std::string::npos && ug.find("host_save=1\n") != std::string::npos && ug.find("automation") == std::string::npos &&
              ug.find("sandbox_saves") == std::string::npos && ug.find("udp_port_local=27556\n") != std::string::npos &&
              ug.rfind("# Generated by Stranded Together", 0) != 0,
          "guest unredirected: role=guest host_save=1, no automation or sandbox key");
}

int wmain(int argc, wchar_t** argv) {
    std::wstring scenarios, supervisor, fakegame, mod, scenario, work = L".";
    bool e2e = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        const auto next = [&]() { return i + 1 < argc ? std::wstring(argv[++i]) : std::wstring(); };
        if (a == L"--scenarios") scenarios = next();
        else if (a == L"--e2e") e2e = true;
        else if (a == L"--supervisor") supervisor = next();
        else if (a == L"--fakegame") fakegame = next();
        else if (a == L"--mod") mod = next();
        else if (a == L"--scenario") scenario = next();
        else if (a == L"--work") work = next();
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (e2e) {
        const int rc = EndToEnd(te::FullPath(supervisor), te::FullPath(fakegame), te::FullPath(mod), te::FullPath(scenario), work);
        if (rc == 77) return rc;
        std::printf("%s: %d passed, %d failed\n", g_fail ? "FAILED" : "PASSED", g_pass, g_fail);
        return g_fail ? 1 : 0;
    }
    CommandParser();
    ScenarioParser(scenarios.empty() ? L"" : te::FullPath(scenarios));
    QuickstartGoto();
    SessionIni();
    ForeignSaves();
    SandboxRules();
    SaveRedirectMapping();
    BackupRestore(te::FullPath(work));
    SavesGuard(te::FullPath(work));
    std::printf("%s: %d passed, %d failed\n", g_fail ? "FAILED" : "PASSED", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
