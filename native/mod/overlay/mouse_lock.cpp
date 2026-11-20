// See mouse_lock.hpp.
#include "mouse_lock.hpp"

namespace st::overlay::mouselock {

Decision Decide(const State& s) noexcept {
    Decision d;
    const auto release_ours = [&](Why why) {
        d.why = why;
        d.act = s.have_clip && s.ours ? Act::release : Act::keep;
        return d;
    };
    if (!s.enabled) return release_ours(Why::off);
    if (s.automation) {
        d.why = Why::automation;
        return d;
    }
    if (!s.foreground) return release_ours(Why::not_front);
    if (s.iconic) return release_ours(Why::minimised);
    if (s.size_move) return release_ours(Why::moving);
    if (EmptyRect(s.client)) return release_ours(Why::no_client);
    if (!s.engaged) {
        d.why = Why::not_engaged;
        return d;
    }
    if (s.have_clip && RectInside(s.clip, s.client)) {
        d.why = s.ours || SameRect(s.clip, s.client) ? Why::held : Why::game_clip;
        return d;
    }
    d.act = Act::clip;
    d.why = Why::clipping;
    d.rect = s.client;
    return d;
}

const char* WhyText(Why why) noexcept {
    switch (why) {
        case Why::off: return "the lock is off";
        case Why::automation: return "hidden run: not applied";
        case Why::not_front: return "another window is in front";
        case Why::minimised: return "the game window is minimised";
        case Why::moving: return "the window is being moved or resized";
        case Why::no_client: return "the window has no client area";
        case Why::not_engaged: return "in front, waiting for the cursor to enter the window";
        case Why::game_clip: return "the game's own clip (inside the window) is in effect";
        case Why::held: return "the cursor is held inside the game window";
        case Why::clipping: return "clipping the cursor to the game window";
    }
    return "?";
}

bool ReplaceGameRelease(const State& s) noexcept {
    return s.enabled && !s.automation && s.foreground && !s.iconic && !s.size_move && s.engaged && !EmptyRect(s.client);
}

std::uint32_t SwitchKey(std::uint32_t menu_vk) noexcept { return menu_vk == VK_F10 ? VK_F11 : VK_F10; }

}  // namespace st::overlay::mouselock
