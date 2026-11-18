// session.ini self_voice (docs/audio-research.md section 9, Phase 1): what THIS player hears of
// Sam's own voice (spoken lines and the non-verbal grunts, efforts, breathing) on their own game.
//
//   auto  mute when this player's character is not Sam (a guest playing Fragile), else Sam's voice
//   sam   Sam's voice as the game plays it (the mod's behaviour when the key is absent)
//   mute  always mute (a test value; the launcher writes auto or sam)
//
// Pure (no Windows or game dependencies): shared by the mod (config.cpp, self_voice.cpp), the
// launcher (core_text.cpp) and the unit tests (st_net_tests, st_launcher_core_tests).
#pragma once
#include <cstring>
#include <string>

namespace st::selfvoice {

enum class Mode : unsigned char { automatic, sam, mute };

// "auto" | "sam" | "mute", exactly (lower case, as the launcher writes them). False (out unchanged) otherwise.
inline bool ParseMode(const std::string& text, Mode* out) noexcept {
    Mode m;
    if (text == "auto") m = Mode::automatic;
    else if (text == "sam") m = Mode::sam;
    else if (text == "mute") m = Mode::mute;
    else return false;
    if (out) *out = m;
    return true;
}

inline const char* ModeName(Mode m) noexcept {
    switch (m) {
        case Mode::automatic: return "auto";
        case Mode::mute: return "mute";
        default: return "sam";
    }
}

// A body_table key that is Sam himself ("sam", "sam_mule", ...: "sam" or "sam_" + anything), any case.
// nullptr / "" count as Sam (the mod sends sam for an unset my_character).
inline bool IsSamCharacter(const char* key) noexcept {
    if (!key || !*key) return true;
    const char lower[4] = {static_cast<char>(key[0] | 0x20), key[1] ? static_cast<char>(key[1] | 0x20) : '\0',
                           key[1] && key[2] ? static_cast<char>(key[2] | 0x20) : '\0', '\0'};
    if (std::strcmp(lower, "sam") != 0) return false;
    return key[3] == '\0' || key[3] == '_';
}

// The setting in force: true = mute Sam's voice for this player. `character` is the body key this
// player is shown as (bodies::ChooseOwn: the host is always sam).
inline bool ResolveMute(Mode m, const char* character) noexcept {
    if (m == Mode::mute) return true;
    if (m == Mode::sam) return false;
    return !IsSamCharacter(character);
}

}  // namespace st::selfvoice
