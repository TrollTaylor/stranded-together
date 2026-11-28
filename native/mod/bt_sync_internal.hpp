// Round 31 (bt_feature.hpp): what the seven BT feature modules borrow from bt_sync.cpp, beyond bt_probe_internal.hpp (included
// here: NowMs, LocalPos, SamFacing, LoadingNow, PlayingWhy, Gazers / GzInfo, GazerKillOk, VoidOk, VoidPrologueOk, VoidHookIn,
// InstallGz, MhInit, FlowGate, FlowCalls). All defined at the end of bt_sync.cpp. Game thread only unless a line says otherwise.
//
// The game calls themselves (the rain spot, the tar list, the blast, the crater append, the mirror writes, the sky call, ...) are
// proven in bt_probe_r31.cpp (ops 15..36): copy those code paths, their byte checks (sites and RIP targets) and their SEH
// wrappers into the module as they are; do not edit bt_probe_r31.cpp (it stays the stage-0 reference) and do not call into it.
#pragma once

#include <cstddef>
#include <cstdint>

#include "bt_feature.hpp"
#include "bt_probe_internal.hpp"

namespace st::bt::internal {

// ---- addresses (any thread)
std::uintptr_t Addr(std::uintptr_t va) noexcept;       // a ds.exe DC VA (preferred base 0x140000000) at the running base
unsigned long long Va(std::uintptr_t addr) noexcept;  // the other way (0 for 0): log lines print VAs
// Byte checks (guarded reads): the code at `va` matches sig ("48 8B ?? 05 ...", '??' any byte); the RIP-relative operand of the
// instruction at insn (rel32 at insn + off, the instruction len bytes long) points at want_va.
bool SiteOk(std::uintptr_t va, const char* sig) noexcept;
bool RipOk(std::uintptr_t insn_va, unsigned off, unsigned len, std::uintptr_t want_va) noexcept;

// ---- guarded memory (any thread): a stale address fails, never raises (the game's VEH crash-reports any AV, even one caught)
bool ReadN(std::uintptr_t addr, void* out, std::size_t n) noexcept;   // ReadProcessMemory on our own process
bool WriteN(std::uintptr_t addr, const void* p, std::size_t n) noexcept;  // WriteProcessMemory on our own process (addr >= 0x10000)
template <class T>
bool Read(std::uintptr_t addr, T& out) noexcept {
    return ReadN(addr, &out, sizeof(T));
}
template <class T>
bool Write(std::uintptr_t addr, const T& v) noexcept {
    return WriteN(addr, &v, sizeof(T));
}

// ---- the managers (0: unreadable, null, or its group failed bt_sync's byte checks; the first call runs those checks once)
std::uintptr_t GazerMgr() noexcept;    // [0x147BEBEC0] DSGazerManager (group kMgr)
std::uintptr_t HunterMgr() noexcept;   // [0x147BECBD0] the hunter manager (group kHunter)
std::uintptr_t CatcherMgr() noexcept;  // [0x147BEE060] the catcher manager (group kCatcher)
std::uintptr_t Registry() noexcept;    // [0x147BBB8C0] the actor registry (group kReg)
std::uintptr_t WeatherSys() noexcept;  // [0x147BF83B8] the WeatherSystem ("ws"; its RIP load at 0x142E1FE34 checked once)
std::uintptr_t ResolveActor(std::uint32_t id) noexcept;  // an actor id through the registry's groups (0: none / not in use)

// ---- the session (game thread)
int SessionRole() noexcept;    // session.ini's role: 1 host, 2 guest, 0 another (solo, echo), whatever bt_sync says
int SyncLevel() noexcept;      // bt_sync here (0..2)
bool RealSave() noexcept;      // this game is a guest on its own real save (host_save=0)
bool RecordsEqual() noexcept;  // both games' BT records equal (bt_sync's FIELD compare; false before both censuses)
bool PeerOn() noexcept;        // the partner's hello arrived this link session
PeerHello Peer() noexcept;     // the partner's hello (zeros while !PeerOn())
bool Stopped() noexcept;       // EmergencyStop ran (any thread)
std::uint32_t Seq() noexcept;  // the next BTS1 sequence number (the TickCtx's seq)
bool SendBts(const std::uint8_t* msg, std::size_t n) noexcept;  // the TickCtx's send
bool PartnerPos(double (&out)[3]) noexcept;  // the partner's body, else the newest remote sample
// bt_sync's rate-limited toast (one per 8 s across the BT sync: coop::SetVehicleNotice for 5 s).
void Toast(double now_ms, const char* text) noexcept;

}  // namespace st::bt::internal
