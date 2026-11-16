// File-based automation channel for hidden test runs. See automation.hpp and
// docs/hidden-testing.md. Runs on its own thread; never touches game memory.
#include "automation.hpp"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <new>
#include <string>
#include <vector>

#include "MinHook.h"
#include "debug_actions.hpp"
#include "log.hpp"
#include "overlay/overlay.hpp"
#include "overlay/overlay_input.hpp"
#include "overlay/overlay_renderer.hpp"

namespace st::automation {
namespace {

std::atomic<bool> g_enabled{false};
std::atomic<bool> g_mute{true};
std::atomic<bool> g_isolate{true};
std::atomic<bool> g_started{false};
std::wstring g_in, g_out;
std::string g_build;
std::atomic<unsigned long> g_commands{0}, g_failures{0};

// ------------------------------------------------------------------ files
std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

bool WriteAtomic(const std::wstring& path, const std::string& text) {
    const std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const bool ok = WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr) && wrote == text.size();
    CloseHandle(f);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

void Append(const std::wstring& path, const std::string& line) {
    HANDLE f = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    WriteFile(f, line.data(), static_cast<DWORD>(line.size()), &wrote, nullptr);
    CloseHandle(f);
}

bool ReadText(const std::wstring& path, std::string* out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart >= 0 && size.QuadPart < (1 << 20);
    if (ok) {
        out->assign(static_cast<std::size_t>(size.QuadPart), '\0');
        DWORD got = 0;
        ok = size.QuadPart == 0 || (ReadFile(f, out->data(), static_cast<DWORD>(size.QuadPart), &got, nullptr) && got == size.QuadPart);
    }
    CloseHandle(f);
    return ok;
}

double NowMs() {
    static LARGE_INTEGER f = [] {
        LARGE_INTEGER q;
        QueryPerformanceFrequency(&q);
        return q;
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}

// ------------------------------------------------------------------ audio
// Hidden runs are silent: every buffer the game hands to WASAPI (Wwise and
// XAudio2 both end in IAudioRenderClient::ReleaseBuffer) is flagged SILENT.
// Process-local; nothing is persisted (no volume-mixer change).
using ReleaseBufferFn = HRESULT(STDMETHODCALLTYPE*)(IAudioRenderClient*, UINT32, DWORD);
ReleaseBufferFn g_orig_release = nullptr;
std::atomic<unsigned long> g_silenced{0};

HRESULT STDMETHODCALLTYPE HookReleaseBuffer(IAudioRenderClient* client, UINT32 frames, DWORD flags) {
    g_silenced.fetch_add(1, std::memory_order_relaxed);
    return g_orig_release(client, frames, flags | AUDCLNT_BUFFERFLAGS_SILENT);
}

void* FindReleaseBuffer(char* why, std::size_t cap) {
    void* target = nullptr;
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    WAVEFORMATEX* fmt = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
    const char* step = "MMDeviceEnumerator";
    if (SUCCEEDED(hr)) step = "GetDefaultAudioEndpoint", hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
    if (SUCCEEDED(hr)) step = "Activate(IAudioClient)", hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
    if (SUCCEEDED(hr)) step = "GetMixFormat", hr = client->GetMixFormat(&fmt);
    if (SUCCEEDED(hr)) step = "Initialize", hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, fmt, nullptr);
    if (SUCCEEDED(hr)) step = "GetService(IAudioRenderClient)", hr = client->GetService(IID_PPV_ARGS(&render));
    if (SUCCEEDED(hr) && render) target = (*reinterpret_cast<void***>(render))[4];  // IUnknown(3), GetBuffer, ReleaseBuffer
    else std::snprintf(why, cap, "%s failed (hr=0x%08lX)", step, static_cast<unsigned long>(hr));
    if (render) render->Release();
    if (fmt) CoTaskMemFree(fmt);
    if (client) client->Release();
    if (dev) dev->Release();
    if (en) en->Release();
    return target;
}

void InstallMute() {
    // COM on a short-lived thread of our own: the bootstrap thread's apartment stays untouched.
    // The context lives on the heap: when the audio stack hangs past the wait below,
    // the thread still writes into it after this function returned (it is leaked then).
    struct Ctx {
        char why[160] = "no audio device";
        void* target = nullptr;
    };
    Ctx* ctx = new (std::nothrow) Ctx;
    if (!ctx) return;
    HANDLE t = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
        auto* c = static_cast<Ctx*>(p);
        if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
            c->target = FindReleaseBuffer(c->why, sizeof(c->why));
            CoUninitialize();
        }
        return 0;
    }, ctx, 0, nullptr);
    if (!t) {
        Log("[auto] audio not muted: no thread (error %lu)", GetLastError());
        delete ctx;
        return;
    }
    const bool finished = WaitForSingleObject(t, 5000) == WAIT_OBJECT_0;
    CloseHandle(t);
    if (!finished) {  // ctx stays allocated: the thread may still write into it
        Log("[auto] audio not muted: the audio device did not answer within 5 s");
        return;
    }
    char why[160];
    std::snprintf(why, sizeof(why), "%s", ctx->why);
    void* const target = ctx->target;
    delete ctx;
    if (!target) {
        Log("[auto] audio not muted: %s", why);
        return;
    }
    const MH_STATUS init = MH_Initialize();
    MH_STATUS s = init == MH_OK || init == MH_ERROR_ALREADY_INITIALIZED ? MH_OK : init;
    if (s == MH_OK) s = MH_CreateHook(target, reinterpret_cast<void*>(&HookReleaseBuffer), reinterpret_cast<void**>(&g_orig_release));
    if (s == MH_OK) s = MH_EnableHook(target);
    if (s == MH_OK) Log("[auto] audio muted for this run (IAudioRenderClient::ReleaseBuffer -> SILENT)");
    else Log("[auto] audio not muted: hook failed (%s)", MH_StatusToString(s));
}

// ------------------------------------------------------------------ game window
HWND GameWindow() {
    HWND h = overlay::input::Window();
    if (h && IsWindow(h)) return h;
    struct Find {
        HWND best = nullptr;
        LONG area = 0;
    } f;
    EnumWindows([](HWND w, LPARAM lp) -> BOOL {
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid != GetCurrentProcessId() || GetWindow(w, GW_OWNER) || !IsWindowVisible(w)) return TRUE;
        RECT r{};
        GetWindowRect(w, &r);
        const LONG area = (r.right - r.left) * (r.bottom - r.top);
        auto* find = reinterpret_cast<Find*>(lp);
        if (area > find->area) find->best = w, find->area = area;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&f));
    return f.best;
}

// ------------------------------------------------------------------ commands
struct Outcome {
    bool ok = false;
    std::string detail;
};

std::atomic<bool> g_quitting{false};

bool SleepFor(std::uint32_t ms) {
    const double end = NowMs() + ms;
    while (NowMs() < end) {
        const double left = end - NowMs();
        Sleep(static_cast<DWORD>(left > 50 ? 50 : (left > 1 ? left : 1)));
    }
    return true;
}

Outcome Fmt(bool ok, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return {ok, buf};
}

Outcome DoScreenshot(const Command& c) {
    if (!overlay::Installed()) return Fmt(false, "no overlay (debug_menu=0 or install failed): nothing can capture");
    const double t0 = NowMs();
    const std::uint32_t id = overlay::RequestCapture();
    if (!id) return Fmt(false, "too many captures pending");
    overlay::Image img;
    char why[200] = {};
    int r = 0;
    while ((r = overlay::TakeCapture(id, &img, why, sizeof(why))) == 0) {
        if (NowMs() - t0 > 20000) {
            overlay::CancelCapture(id);  // frees the slot: four timeouts must not block every later screenshot
            return Fmt(false, "no frame presented within 20 s (game not rendering?)");
        }
        Sleep(15);
    }
    if (r < 0) return Fmt(false, "capture failed: %s", why);
    const std::wstring path = g_out + L"\\" + Widen(c.text) + L".png";
    if (!overlay::WritePngFile(path, img)) return Fmt(false, "could not write %s.png", c.text.c_str());
    // Mean brightness: a quick hint whether the frame is black.
    unsigned long long sum = 0;
    const std::size_t n = img.rgb.size();
    for (std::size_t i = 0; i < n; i += 97) sum += img.rgb[i];
    const double mean = n ? static_cast<double>(sum) / static_cast<double>((n + 96) / 97) : 0;
    return Fmt(true, "%s.png %ux%u mean=%.1f after %.0f ms", c.text.c_str(), img.width, img.height, mean, NowMs() - t0);
}

Outcome DoPad(const Command& c) {
    const overlay::input::Stats is = overlay::input::GetStats();
    if (!overlay::Installed() || !is.xinput_hook)
        return Fmt(false, "virtual pad unavailable (overlay not installed or XInputGetState not hooked)");
    overlay::input::PadState p;
    p.connected = true;
    p.buttons = c.buttons;
    p.lx = c.lx, p.ly = c.ly, p.rx = c.rx, p.ry = c.ry, p.lt = c.lt, p.rt = c.rt;
    const unsigned long reads0 = is.virtual_pad_reads;
    overlay::input::SetVirtualPad(p);
    SleepFor(c.ms);
    overlay::input::SetVirtualPad(overlay::input::PadState{true});  // idle, still connected
    const unsigned long reads = overlay::input::GetStats().virtual_pad_reads - reads0;
    return Fmt(true, "buttons=0x%04X held %u ms; the game read the pad %lu times", c.buttons, c.ms, reads);
}

Outcome DoKey(const Command& c) {
    const HWND w = GameWindow();
    if (!w) return Fmt(false, "no game window yet");
    const UINT scan = MapVirtualKeyW(c.vk, MAPVK_VK_TO_VSC);
    const LPARAM down = 1 | (static_cast<LPARAM>(scan) << 16);
    const LPARAM up = down | (1LL << 30) | (1LL << 31);
    PostMessageW(w, WM_KEYDOWN, c.vk, down);
    SleepFor(c.ms);
    PostMessageW(w, WM_KEYUP, c.vk, up);
    return Fmt(true, "vk 0x%02X posted to window %p (window procedure only, not WH_KEYBOARD)", c.vk, static_cast<void*>(w));
}

Outcome DoMenu(const Command& c) {
    if (!overlay::Installed()) return Fmt(false, "no overlay");
    const bool want = c.text == "open" ? true : c.text == "close" ? false : !overlay::IsOpen();
    overlay::SetOpen(want);
    const double t0 = NowMs();
    while (overlay::IsOpen() != want) {
        if (NowMs() - t0 > 5000) return Fmt(false, "menu still %s after 5 s (no Present?)", want ? "closed" : "open");
        Sleep(15);
    }
    return Fmt(true, "menu %s", want ? "open" : "closed");
}

Outcome DoAction(const Command& c) {
    std::size_t n = 0;
    const debug::ActionInfo* rows = debug::Actions(&n);
    const debug::ActionInfo* found = nullptr;
    std::string names;
    for (std::size_t i = 0; i < n; ++i) {
        const std::string norm = NormalizeActionName(rows[i].label ? rows[i].label : "");
        if (norm == NormalizeActionName(c.text)) found = &rows[i];
        names += (names.empty() ? "" : ", ") + norm;
    }
    if (!found) return Fmt(false, "unknown action '%s' (known: %s)", c.text.c_str(), names.c_str());
    debug::Request r;
    r.action = found->id;
    for (std::size_t i = 0; i < c.numbers.size() && i < 3; ++i) r.v[i] = c.numbers[i];
    for (std::size_t i = 0; i < c.numbers.size() && i < 2; ++i) r.f[i] = static_cast<float>(c.numbers[i]);
    if (c.numbers.size() == 1) r.i = static_cast<std::int32_t>(c.numbers[0]);  // voice_ping <line>
    r.flag = !c.numbers.empty() && c.numbers[0] != 0;
    const std::uint32_t serial = debug::Queue(r);
    if (!serial) return Fmt(false, "action queue full");
    const double t0 = NowMs();
    debug::Result res;
    while (!debug::ResultFor(serial, &res)) {
        if (NowMs() - t0 > 15000) return Fmt(false, "'%s' queued but not run within 15 s (no game tick?)", found->label);
        Sleep(15);
    }
    return Fmt(res.ok, "%s: %s", found->label, res.text);
}

std::string SnapshotText() {
    const debug::Snapshot s = debug::GetSnapshot();
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "serial=%llu role=%s mod_state=\"%s\" local_present=%d local_valid=%d pos=(%.2f, %.2f, %.2f) speed=%.2f "
                  "partner=%s link=%s tick_hz=%.1f ticks=%llu",
                  static_cast<unsigned long long>(s.serial), s.role, s.mod_state, s.local_present ? 1 : 0, s.local_valid ? 1 : 0,
                  s.local_pos[0], s.local_pos[1], s.local_pos[2], static_cast<double>(s.speed), s.partner_state, s.link_state,
                  static_cast<double>(s.tick_hz), static_cast<unsigned long long>(s.ticks));
    return buf;
}

Outcome DoWaitPlayer(const Command& c) {
    const double t0 = NowMs();
    for (;;) {
        const debug::Snapshot s = debug::GetSnapshot();
        if (s.local_valid)
            return Fmt(true, "local player at (%.2f, %.2f, %.2f) after %.1f s", s.local_pos[0], s.local_pos[1], s.local_pos[2],
                       (NowMs() - t0) / 1000.0);
        if (NowMs() - t0 > c.ms) return Fmt(false, "no local player within %u s (%s)", c.ms / 1000, SnapshotText().c_str());
        Sleep(100);
    }
}

Outcome DoWaitLog(const Command& c) {
    std::uint64_t after = LogNewestSeq();
    const double t0 = NowMs();
    static LogLine lines[64];  // automation thread only
    for (;;) {
        std::size_t n;
        while ((n = LogCopyRecent(after, lines, 64)) > 0) {
            for (std::size_t i = 0; i < n; ++i) {
                after = lines[i].seq;
                // our own "[auto]" lines echo the needle; never match them
                if (!std::strstr(lines[i].text, "[auto]") && std::strstr(lines[i].text, c.text.c_str()))
                    return Fmt(true, "%s", lines[i].text);
            }
        }
        if (NowMs() - t0 > c.ms) return Fmt(false, "no new log line with '%s' within %u s", c.text.c_str(), c.ms / 1000);
        Sleep(50);
    }
}

void DoQuit() {
    g_quitting.store(true);
    const HWND w = GameWindow();
    Log("[auto] quit: WM_CLOSE to the game window %p; the process ends within 20 s", static_cast<void*>(w));
    if (w) PostMessageW(w, WM_CLOSE, 0, 0);
    HANDLE t = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        Sleep(20000);
        Log("[auto] quit: the game did not exit after WM_CLOSE: ending the process");
        LogShutdown();
        TerminateProcess(GetCurrentProcess(), 0);
        return 0;
    }, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}

Outcome Execute(const Command& c) {
    switch (c.kind) {
        case Kind::wait: SleepFor(c.ms); return Fmt(true, "%u ms", c.ms);
        case Kind::log: Log("[auto] %s", c.text.c_str()); return Fmt(true, "logged");
        case Kind::screenshot: return DoScreenshot(c);
        case Kind::pad: return DoPad(c);
        case Kind::padoff: overlay::input::SetVirtualPad(overlay::input::PadState{}); return Fmt(true, "virtual pad disconnected");
        case Kind::key: return DoKey(c);
        case Kind::menu: return DoMenu(c);
        case Kind::action: return DoAction(c);
        case Kind::waitplayer: return DoWaitPlayer(c);
        case Kind::waitlog: return DoWaitLog(c);
        case Kind::snapshot: return Fmt(true, "%s", SnapshotText().c_str());
        case Kind::quit: return Fmt(true, "quitting");
        default: return Fmt(false, "unknown command");
    }
}

void OneLine(std::string& s) {
    for (char& ch : s)
        if (ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
}

void RunFile(const std::wstring& name) {
    const std::wstring path = g_in + L"\\" + name;
    std::string text;
    if (!ReadText(path, &text)) return;
    DeleteFileW(path.c_str());  // taken
    const std::wstring base = name.substr(0, name.size() - 4);  // without ".cmd"
    const std::string file = Narrow(base);
    bool all_ok = true;
    std::string last;
    int lineno = 0, ran = 0;
    bool quit = false;
    std::size_t at = 0;
    while (at <= text.size() && !quit) {
        std::size_t nl = text.find('\n', at);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(at, nl - at);
        at = nl + 1;
        ++lineno;
        Command c;
        std::string err;
        Outcome o;
        if (!ParseCommand(line, &c, &err)) {
            if (err.empty()) continue;  // blank / comment
            o = {false, err};
        } else {
            const double t0 = NowMs();
            try {
                o = Execute(c);
            } catch (...) {
                o = {false, "exception in the automation thread"};
            }
            OneLine(o.detail);
            char took[32];
            std::snprintf(took, sizeof(took), " (%.0f ms)", NowMs() - t0);
            o.detail += took;
            quit = c.kind == Kind::quit;
        }
        ++ran;
        g_commands.fetch_add(1);
        if (!o.ok) all_ok = false, g_failures.fetch_add(1);
        std::string trimmed = detail::Trim(line);
        OneLine(trimmed);
        Log("[auto] %s:%d %s -> %s: %s", file.c_str(), lineno, trimmed.c_str(), o.ok ? "ok" : "FAIL", o.detail.c_str());
        Append(g_out + L"\\results.txt", file + ":" + std::to_string(lineno) + "\t" + (o.ok ? "ok" : "fail") + "\t" + trimmed +
                                             "\t" + o.detail + "\n");
        last = o.detail;
        if (!o.ok) break;  // a failed line ends its file
    }
    WriteAtomic(g_out + L"\\" + base + L".done", std::string(all_ok ? "ok" : "fail") + "\n" + std::to_string(ran) + "\n" + last + "\n");
    if (quit) DoQuit();
}

void WriteStatus(double started) {
    const overlay::Stats os = overlay::GetStats();
    const overlay::input::Stats is = overlay::input::GetStats();
    const overlay::input::PadState pad = overlay::input::VirtualPad();
    char buf[1400];
    std::snprintf(buf, sizeof(buf),
                  "pid=%lu\nuptime_s=%.1f\ncommands=%lu\nfailures=%lu\noverlay_installed=%d\noverlay_disabled=%d\n"
                  "overlay_reason=%s\npresents=%lu\nfps=%.1f\nbackbuffer=%ux%u\nformat=%d\nqueue=%s\nmenu_open=%d\n"
                  "occluded_masked=%lu\nwindow=%p\nxinput_hook=%d\nxinput14_hook=%d\npad_connected=%d\npad_buttons=0x%04X\npad_reads=%lu\n"
                  "isolated_pad_reads=%lu\nforeground_answers=%lu\naudio_buffers_silenced=%lu\nlog_seq=%llu\nsnapshot=%s\n",
                  GetCurrentProcessId(), (NowMs() - started) / 1000.0, g_commands.load(), g_failures.load(),
                  os.installed ? 1 : 0, os.disabled ? 1 : 0, os.disabled_reason, os.presents, static_cast<double>(os.fps),
                  os.width, os.height, os.format, os.queue_source, overlay::IsOpen() ? 1 : 0, os.occluded_masked,
                  static_cast<void*>(overlay::input::Window()), is.xinput_hook ? 1 : 0, is.xinput14_hook ? 1 : 0,
                  pad.connected ? 1 : 0, pad.buttons, is.virtual_pad_reads, is.isolated_pad_reads, is.foreground_answers,
                  g_silenced.load(), static_cast<unsigned long long>(LogNewestSeq()), SnapshotText().c_str());
    WriteAtomic(g_out + L"\\status.txt", buf);
}

DWORD WINAPI Thread(LPVOID) {
    const double started = NowMs();
    double last_status = -1e9;
    for (;;) {
        if (NowMs() - last_status >= 1000) {
            WriteStatus(started);
            last_status = NowMs();
        }
        if (!g_quitting.load()) {
            std::vector<std::wstring> names;
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((g_in + L"\\*.cmd").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) names.emplace_back(fd.cFileName);
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
            std::sort(names.begin(), names.end());
            for (const std::wstring& n : names) {
                if (g_quitting.load()) break;
                try {
                    RunFile(n);
                } catch (...) {
                    Log("[auto] exception while running %s", Narrow(n).c_str());
                }
                if (NowMs() - last_status >= 1000) {
                    WriteStatus(started);
                    last_status = NowMs();
                }
            }
        }
        Sleep(100);
    }
}

}  // namespace

bool Init(const std::wstring& mod_dir) noexcept {
    try {
        std::ifstream in(mod_dir + L"\\session.ini");
        if (!in) return false;
        std::string line;
        bool on = false, mute = true, isolate = true;
        while (std::getline(in, line)) {
            line = detail::Trim(line);
            if (line.empty() || line[0] == '#' || line[0] == ';') continue;
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = detail::Trim(line.substr(0, eq)), v = detail::Lower(detail::Trim(line.substr(eq + 1)));
            const bool b = v == "1" || v == "true" || v == "yes" || v == "on";
            if (k == "automation") on = b;
            else if (k == "automation_mute") mute = b;
            else if (k == "automation_isolate") isolate = b;
        }
        g_enabled.store(on);
        g_mute.store(mute);
        g_isolate.store(isolate);
        if (on)
            Log("[auto] automation=1 in session.ini: hidden test run (screenshots, command files%s%s)",
                isolate ? ", virtual pad, input/display isolation" : "; automation_isolate=0: no isolation hooks", mute ? ", muted" : "");
        return on;
    } catch (...) {
        return false;
    }
}

bool Enabled() noexcept { return g_enabled.load(); }
bool MuteRequested() noexcept { return g_mute.load(); }
bool IsolateRequested() noexcept { return g_isolate.load(); }

void Start(const std::wstring& mod_dir, const char* build) noexcept {
    if (!g_enabled.load() || g_started.exchange(true)) return;
    try {
        g_build = build ? build : "";
        const std::wstring root = mod_dir + L"\\automation";
        g_in = root + L"\\in";
        g_out = root + L"\\out";
        CreateDirectoryW(root.c_str(), nullptr);
        CreateDirectoryW(g_in.c_str(), nullptr);
        CreateDirectoryW(g_out.c_str(), nullptr);
        if (g_mute.load()) InstallMute();
        char ready[256];
        std::snprintf(ready, sizeof(ready), "ready\npid=%lu\nbuild=%s\noverlay=%d\n", GetCurrentProcessId(), g_build.c_str(),
                      overlay::Installed() ? 1 : 0);
        HANDLE t = CreateThread(nullptr, 0, &Thread, nullptr, 0, nullptr);
        if (!t) {
            Log("[auto] could not start the automation thread (error %lu)", GetLastError());
            return;
        }
        CloseHandle(t);
        WriteAtomic(g_out + L"\\ready.txt", ready);
        Log("[auto] channel ready: commands from %s", Narrow(g_in).c_str());
    } catch (...) {
        Log("[auto] automation start failed (exception)");
    }
}

}  // namespace st::automation
