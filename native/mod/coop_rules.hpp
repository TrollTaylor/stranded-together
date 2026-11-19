// The host-held save session's decision rules (docs/coop-session.md section 5), pure and unit-tested
// (tests/save_xfer_tests.cpp). coop_session.cpp feeds them what the game and the link show.
#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "save_xfer.hpp"

namespace st::coop::rules {

// A local player entity counts once it has been the same entity this long: while a world loads the game re-creates it
// two or three times within ~5 s (hidden run 20261119-065746).
inline constexpr double kStableMs = 5000;
// A load request this soon after a detected load, before a new player is stable, belongs to that load.
inline constexpr double kFollowUpMs = 10000;

// The in-world load request 0x14362D750 (type, index) on the host: the game's own follow-up of a load the host's hooks
// already saw (index -2 right after a title Continue, run 20261118-223454), or a load of its own (Retry for -2, an
// in-world Load otherwise)?
inline bool IsFollowUp(bool session_zero, double now_ms, double last_detect_ms, bool player_since_detect, int type, int index,
                       int last_type, int last_index) noexcept {
    if (session_zero) return true;
    const bool fresh = now_ms - last_detect_ms < kFollowUpMs && !player_since_detect;
    return fresh && (index == -2 || (type == last_type && index == last_index));
}

// The host's phase for STATE.
inline stx::HostPhase HostPhaseOf(bool session, bool stable_player, bool load_pending) noexcept {
    if (session && stable_player && !load_pending) return stx::HostPhase::world;
    if (load_pending || session) return stx::HostPhase::loading;
    return stx::HostPhase::title;
}

// The host offers its newest save to a partner without the current world (late join, back at its title after a lost
// link, a failed load): only in the world, with nothing being sent, versions equal, the partner's state fresh, at most
// once per kReofferMs.
inline constexpr double kReofferMs = 30000;
struct LateJoinIn {
    bool connected = false;
    bool have_gstate = false;
    double gstate_age_ms = 1e18;
    stx::HostPhase host_phase = stx::HostPhase::title;
    bool new_game_pending = false;
    bool sender_busy = false;
    bool sender_done = false;            // the current offer reached RESULT ok
    std::uint32_t current_counter = 0;   // the current offer's load_counter
    stx::Sha256 current_sha{};
    bool version_mismatch = false;
    stx::GState gstate;
    double since_last_offer_ms = 1e18;
};
enum class LateJoin : std::uint8_t { none, waiting_partner, older_world };
inline LateJoin WantLateJoin(const LateJoinIn& in) noexcept {
    if (!in.connected || !in.have_gstate || in.gstate_age_ms >= 3000 || in.host_phase != stx::HostPhase::world || in.new_game_pending ||
        in.sender_busy || in.version_mismatch || in.gstate.phase == stx::GuestPhase::refused || in.since_last_offer_ms <= kReofferMs)
        return LateJoin::none;
    if (in.gstate.phase == stx::GuestPhase::waiting) return LateJoin::waiting_partner;
    const bool has_current = in.sender_done && in.gstate.loaded_load_counter == in.current_counter && in.gstate.loaded_sha == in.current_sha;
    if (in.gstate.phase == stx::GuestPhase::world && !has_current) return LateJoin::older_world;
    return LateJoin::none;
}

// On a new link session, an offer nobody answered yet (phase offered) for a title load or a late join is stale once the
// host is in the world: cancelled, and the late-join rule offers the newest save instead. Any other running offer is
// announced again (the guest resumes it).
inline bool StaleOnReconnect(stx::HostPhase host_phase, bool offered_unanswered, stx::Reason reason) noexcept {
    return host_phase == stx::HostPhase::world && offered_unanswered &&
           (reason == stx::Reason::title_continue || reason == stx::Reason::title_load || reason == stx::Reason::late_join);
}

// The guest's GSTATE phase as its game really is: a save loaded earlier but the game back at its title = waiting.
inline stx::GuestPhase GuestPhaseOf(stx::GuestPhase receiver_phase, bool at_title, bool in_world) noexcept {
    return receiver_phase == stx::GuestPhase::world && at_title && !in_world ? stx::GuestPhase::waiting : receiver_phase;
}

// The guest's slots: manualsave5 / manualsave6 alternating (never the one just loaded, never an autosave).
inline int NextGuestSlot(int last) noexcept { return last == 5 ? 6 : 5; }

// The guest's title load is called only once its title menu can be up: the game has no "title menu ready" signal the
// mod reads, so time stands in for it. The title menu comes about 20 s after the game starts (boot logos, "press any
// button", notices such as the offline-mode one) and the mod's first tick is ~5 s after the start, so: kTitleBootMs
// after the first tick, and the session global 0 without a break for kTitleSettleMs (after QuitGame, the title menu is
// rebuilt after the world's teardown). Until then the received save waits. first_tick_ms / title_since_ms: when the
// first tick ran / since when the session has been 0 (both on the same clock as now_ms).
inline constexpr double kTitleBootMs = 25000;
inline constexpr double kTitleSettleMs = 8000;
inline bool TitleReady(double now_ms, double first_tick_ms, bool at_title, double title_since_ms) noexcept {
    return at_title && now_ms - first_tick_ms >= kTitleBootMs && now_ms - title_since_ms >= kTitleSettleMs;
}

// "The newest save" (a Retry: autosaves only; a late join or "offer now": any slot) as the host's CURRENT world allows:
// only slots written at or after that world's load (world_ft, FILETIME; 0 = not known: every slot), newest first, at
// most three (one that cannot be read is skipped for the next). None: the current world's own slot (the one the host
// loaded, world_slot; "" = not known), when it is still on disk (in `slots`), and *own_slot = true. An older world's
// newer-looking autosave is never offered for the world the host plays now.
struct SlotTime {
    std::string name;
    std::uint64_t time = 0;   // checkpoint.dat's last write (FILETIME)
};
inline std::vector<std::string> NewestCandidates(std::vector<SlotTime> slots, bool autosave_only, std::uint64_t world_ft, const std::string& world_slot,
                                                 bool* own_slot = nullptr) {
    if (own_slot) *own_slot = false;
    std::vector<SlotTime> ok;
    bool world_there = false;
    for (const SlotTime& s : slots) {
        if (!world_slot.empty() && s.name == world_slot) world_there = true;
        if (autosave_only && s.name.compare(0, 8, "autosave") != 0) continue;
        if (s.time >= world_ft) ok.push_back(s);
    }
    std::sort(ok.begin(), ok.end(), [](const SlotTime& a, const SlotTime& b) { return a.time != b.time ? a.time > b.time : a.name < b.name; });
    std::vector<std::string> out;
    for (std::size_t i = 0; i < ok.size() && i < 3; ++i) out.push_back(ok[i].name);
    if (out.empty() && world_there) {
        out.push_back(world_slot);
        if (own_slot) *own_slot = true;
    }
    return out;
}

}  // namespace st::coop::rules
