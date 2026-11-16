// st_fakepeer: a console stand-in for the second player, speaking the real
// link v2 protocol over UDP. Use it to test the whole pipeline (handshake ->
// pose frames -> partner spawn -> interpolation) against ONE running copy of
// Death Stranding:
//
//   game  session.ini:  role=host  transport=udp  udp_port_local=27555  udp_port_peer=27556
//   here:  st_fakepeer --role guest --local 27556 --remote 27555
//
// The fake partner orbits your character (radius 3 m, one lap per 8 s) once it
// has heard from you. `--selftest` runs a host and a guest fakepeer against
// each other (two processes) and exits 0 when both exchanged poses.
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "config.hpp"
#include "link.hpp"
#include "log.hpp"
#include "pose_wire.hpp"

namespace {

struct Args {
    std::string role = "guest";
    std::string peer = "127.0.0.1";
    unsigned local = 27556, remote = 27555;
    std::string secret;
    double seconds = 0;  // 0 = forever
    double radius = 3.0;
    bool selftest = false;
};

int Usage() {
    std::printf(
        "usage: st_fakepeer [--role host|guest] [--peer IPv4] [--local PORT] [--remote PORT]\n"
        "                   [--secret 20CHARS] [--seconds N] [--radius M] [--selftest]\n");
    return 2;
}

int RunPeer(const Args& a) {
    st::Config c;
    c.present = true;
    c.role = a.role == "host" ? st::Role::host : st::Role::guest;
    c.transport = st::Transport::udp;
    c.udp_peer = a.peer;
    c.udp_port_local = static_cast<std::uint16_t>(a.local);
    c.udp_port_peer = static_cast<std::uint16_t>(a.remote);
    c.secret = a.secret;
    if (!st::link::Start(c)) return 3;

    const ULONGLONG t0 = st::link::NowMs();
    ULONGLONG next_print = t0 + 1000;
    std::uint32_t seq = 0;
    bool have_center = false;
    double cx = 0, cy = 0, cz = 0;
    unsigned received = 0;
    bool ever_connected = false;
    for (;;) {
        const ULONGLONG now = st::link::NowMs();
        st::wire::Pose in;
        std::uint64_t recv = 0;
        std::uint32_t gen = 0;
        while (st::link::PopPose(in, recv, gen)) {
            ++received;
            cx = in.x, cy = in.y, cz = in.z;
            have_center = true;
        }
        // Orbit the other player (Decima is Z-up: circle in X/Y).
        const double t = (now - t0) / 1000.0;
        const double ang = t * 2.0 * 3.14159265 / 8.0;
        st::wire::Pose out;
        out.seq = ++seq;
        out.sender_ms = static_cast<std::uint32_t>(now);
        out.flags = st::wire::kPoseValid;
        out.x = cx + a.radius * std::cos(ang);
        out.y = cy + a.radius * std::sin(ang);
        out.z = cz;
        // Face along the tangent: yaw about Z.
        const float yaw = static_cast<float>(ang + 3.14159265 / 2.0);
        out.rot = st::wire::Quat{0, 0, std::sin(yaw / 2), std::cos(yaw / 2)};
        out.speed = static_cast<float>(a.radius * 2.0 * 3.14159265 / 8.0);
        if (have_center || c.role == st::Role::host) st::link::SendPose(out);

        if (st::link::GetInfo().connected) ever_connected = true;
        if (now >= next_print) {
            next_print = now + 1000;
            const st::link::Info i = st::link::GetInfo();
            std::printf("[fakepeer %s] link=%s peer=%s rtt=%ums rx=%llu tx=%llu partner_at=(%.1f, %.1f, %.1f)%s\n",
                        a.role.c_str(), i.state, i.peer, i.rtt_ms, static_cast<unsigned long long>(i.rx_frames),
                        static_cast<unsigned long long>(i.tx_frames), cx, cy, cz, have_center ? "" : " (no poses yet)");
            std::fflush(stdout);
        }
        if (a.seconds > 0 && (now - t0) / 1000.0 >= a.seconds) break;
        Sleep(33);
    }
    st::link::Shutdown();
    Sleep(50);
    const bool ok = ever_connected && received >= 10;
    std::printf("[fakepeer %s] done: connected=%d received=%u -> %s\n", a.role.c_str(), ever_connected ? 1 : 0, received,
                ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}

int SelfTest(const char* exe) {
    // Child = guest on 27612 -> host on 27611.
    std::string cmd = std::string("\"") + exe + "\" --role guest --local 27612 --remote 27611 --seconds 6";
    STARTUPINFOA si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        std::printf("selftest: could not start the guest process\n");
        return 4;
    }
    Args host;
    host.role = "host";
    host.local = 27611;
    host.remote = 27612;
    host.seconds = 6;
    const int host_rc = RunPeer(host);
    WaitForSingleObject(pi.hProcess, 15000);
    DWORD guest_rc = 1;
    GetExitCodeProcess(pi.hProcess, &guest_rc);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    std::printf("selftest: host=%d guest=%lu -> %s\n", host_rc, guest_rc, host_rc == 0 && guest_rc == 0 ? "PASS" : "FAIL");
    return host_rc == 0 && guest_rc == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        auto next = [&](void) -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (k == "--role") a.role = next();
        else if (k == "--peer") a.peer = next();
        else if (k == "--local") a.local = static_cast<unsigned>(std::atoi(next()));
        else if (k == "--remote") a.remote = static_cast<unsigned>(std::atoi(next()));
        else if (k == "--secret") a.secret = next();
        else if (k == "--seconds") a.seconds = std::atof(next());
        else if (k == "--radius") a.radius = std::atof(next());
        else if (k == "--selftest") a.selftest = true;
        else return Usage();
    }
    st::LogToStdout(true);
    if (a.selftest) return SelfTest(argv[0]);
    if (a.role != "host" && a.role != "guest") return Usage();
    return RunPeer(a);
}
