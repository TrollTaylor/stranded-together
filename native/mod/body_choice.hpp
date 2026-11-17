// Which real character body each player sends and shows (docs/real-partner-body.md
// "Character choice"). Pure code on top of body_table (no Windows or game
// dependencies; unit-tested in st_net_tests).
//
//   Sender:   session.ini my_character is the body THIS player appears as on the other
//             player's screen; it goes out as STJ1 body_id. The host is always Sam.
//   Receiver: shows the peer's body_id when it is an offered body (else Sam), and only
//             switches after the new id has been stable for kStableFrames received
//             frames, so a flapping id never respawns the body over and over.
//             session.ini partner_character is a debug override and wins.
#pragma once
#include <cstdint>

#include "body_table.hpp"

namespace st::bodies {

// About 1 s of STJ1 frames at the default send_hz=30.
inline constexpr unsigned kStableFrames = 30;

struct OwnChoice {
    const Body* body = nullptr;  // never null after ChooseOwn: what goes out as STJ1 body_id
    bool host_forced = false;    // the host's my_character named another body: Sam is sent anyway
    bool unknown_key = false;    // my_character was set but is not in the table: Sam is sent
};
// is_host: role=host. my_character: the session.ini value ("" or nullptr = not set).
// A known body that is not offered is still sent as asked (the receiver shows Sam for it).
OwnChoice ChooseOwn(bool is_host, const char* my_character);

struct PartnerChoice {
    const Body* body = nullptr;     // never null after ChoosePartner
    bool from_override = false;     // partner_character named a body in the table
    bool unknown_override = false;  // partner_character was set but is not in the table (ignored)
    bool peer_not_offered = false;  // the peer's id is unknown or not offered: Sam instead
};
// override_key: session.ini partner_character (debug; any body in the table, offered or
// not, so debug tours can show the unproven ones). peer_known: at least one STJ1 frame
// arrived; without one the peer's body is not known yet and Sam is shown.
PartnerChoice ChoosePartner(const char* override_key, bool peer_known, std::uint8_t peer_body_id);

// Debounce of the peer's body_id. The first id is taken at once (there is nothing to
// flap from); after that a different id must arrive in `frames` consecutive frames.
class StableId {
public:
    explicit StableId(unsigned frames = kStableFrames) : frames_(frames ? frames : 1) {}
    // One received frame's body_id. True when Shown() changed (including the first id).
    bool Push(std::uint8_t id);
    bool Known() const { return known_; }
    std::uint8_t Shown() const { return shown_; }
    void Reset() {
        known_ = false;
        shown_ = candidate_ = 0;
        count_ = 0;
    }

private:
    unsigned frames_;
    bool known_ = false;
    std::uint8_t shown_ = 0;
    std::uint8_t candidate_ = 0;
    unsigned count_ = 0;
};

}  // namespace st::bodies
