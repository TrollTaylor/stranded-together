#include "body_choice.hpp"

namespace st::bodies {

OwnChoice ChooseOwn(bool is_host, const char* my_character) {
    OwnChoice c;
    const bool set = my_character && *my_character;
    const Body* asked = set ? FindByKey(my_character) : nullptr;
    c.unknown_key = set && !asked;
    if (is_host) {
        // The host is always Sam (shared contract with the launcher, whose Host screen says
        // so and writes my_character=sam); a hand-edited session.ini cannot change that.
        c.body = &Default();
        c.host_forced = asked && asked != &Default();
        return c;
    }
    c.body = asked ? asked : &Default();
    return c;
}

PartnerChoice ChoosePartner(const char* override_key, bool peer_known, std::uint8_t peer_body_id) {
    PartnerChoice c;
    if (override_key && *override_key) {
        if (const Body* b = FindByKey(override_key)) {
            c.body = b;
            c.from_override = true;
            return c;
        }
        c.unknown_override = true;
    }
    if (peer_known && IsOffered(peer_body_id)) {
        c.body = FindById(peer_body_id);
        return c;
    }
    c.peer_not_offered = peer_known;
    c.body = &Default();
    return c;
}

bool StableId::Push(std::uint8_t id) {
    if (!known_) {
        known_ = true;
        shown_ = id;
        count_ = 0;
        return true;
    }
    if (id == shown_) {
        count_ = 0;  // back to the shown id: a short excursion never adds up
        return false;
    }
    if (id != candidate_) {
        candidate_ = id;
        count_ = 0;
    }
    if (++count_ < frames_) return false;
    shown_ = id;
    count_ = 0;
    return true;
}

}  // namespace st::bodies
