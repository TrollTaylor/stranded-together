// Stranded Together launcher core: session text (see core_text.hpp).
// The secret, SteamID, INI and redaction helpers are adapted from Diamond Dogs
// Together's core_text.cpp; session.ini and the status view are new.
#include "core_text.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "body_table.hpp"   // native/mod, pure data: the character list shared with the mod
#include "core_util.hpp"

namespace st::core {
namespace {

bool InAlphabet(char c) { return c != 0 && std::strchr(kAlphabet, c) != nullptr; }

bool IsWordChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}
bool IsDigit(char c) { return c >= '0' && c <= '9'; }
bool IsBlank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }
bool IsAlnumAscii(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || IsDigit(c); }

bool ParseIntStrict(const std::string& text, int lo, int hi, int& out) {
    if (text.empty() || text.size() > 9) return false;
    long v = 0;
    for (char c : text) {
        if (!IsDigit(c)) return false;
        v = v * 10 + (c - '0');
    }
    if (v < lo || v > hi) return false;
    out = static_cast<int>(v);
    return true;
}

bool ParseDoubleStrict(const std::string& text, double lo, double hi, double& out) {
    if (text.empty() || text.size() > 16) return false;
    char* end = nullptr;
    const double v = std::strtod(text.c_str(), &end);
    if (!end || *end != '\0' || !(v >= lo && v <= hi)) return false;
    out = v;
    return true;
}

// Short decimal text for a double: "2", "1.5", "0.25".
std::string ShortDecimal(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", v);
    std::string s(buf);
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s.empty() ? std::string("0") : s;
}

bool IniBool(const std::string& v) { return v == "1" || v == "true" || v == "yes" || v == "on"; }

}  // namespace

int ParseFunctionKey(const std::string& text) {
    const std::string t = Trim(text);
    if (t.size() < 2 || t.size() > 3 || (t[0] != 'F' && t[0] != 'f')) return 0;
    int n = 0;
    for (std::size_t i = 1; i < t.size(); ++i) {
        if (!IsDigit(t[i])) return 0;
        n = n * 10 + (t[i] - '0');
    }
    if (t[1] == '0') return 0;
    return n >= 1 && n <= 12 ? n : 0;
}

std::string FunctionKeyName(int key) { return key >= 1 && key <= 12 ? "F" + std::to_string(key) : std::string(); }

// ---- Secrets, Steam IDs -------------------------------------------------------
bool IsCanonicalSecret(const std::string& secret) {
    if (secret.size() != 20) return false;
    for (char c : secret)
        if (!InAlphabet(c)) return false;
    return true;
}

std::string NewSecret() {
    std::uint8_t bytes[20]{};
    if (!RandomBytes(bytes, sizeof(bytes))) return {};
    std::string out;
    for (std::uint8_t b : bytes) out.push_back(kAlphabet[b & 31]);
    return out;
}

bool IsIndividualSteamId64(const std::string& text) {
    if (text.size() != 17 || text[0] < '1' || text[0] > '9') return false;
    std::uint64_t id = 0;
    for (char c : text) {
        if (!IsDigit(c)) return false;
        id = id * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return (id >> 32) == 0x01100001ull && (id & 0xFFFFFFFFull) != 0;
}

std::string SteamId64FromAccount(std::uint32_t account) {
    if (account == 0) return {};
    return Decimal(kSteamIdBase + account);
}

bool IsCanonicalIPv4(const std::string& text) {
    int parts = 0;
    std::size_t i = 0;
    while (parts < 4) {
        const std::size_t start = i;
        int octet = 0;
        while (i < text.size() && IsDigit(text[i]) && i - start < 3) octet = octet * 10 + (text[i++] - '0');
        if (i == start || octet > 255 || (i - start > 1 && text[start] == '0')) return false;
        ++parts;
        if (parts < 4) {
            if (i >= text.size() || text[i] != '.') return false;
            ++i;
        }
    }
    return i == text.size();
}

bool IsValidPlayerName(const std::string& name) {
    if (name.empty() || name.size() > 32) return false;
    if (name.front() == ' ' || name.back() == ' ') return false;
    for (char c : name) {
        if (c < 0x20 || c > 0x7E) return false;
        if (c == '#' || c == ';' || c == '=' || c == '"' || c == '\'') return false;
    }
    return true;
}

// ---- Characters ------------------------------------------------------------------
std::vector<CharacterChoice> OfferedCharacters() {
    std::vector<CharacterChoice> out;
    for (std::size_t i = 0; i < bodies::OfferedCount(); ++i) {
        const bodies::Body& b = bodies::OfferedAt(i);
        out.push_back({b.key, b.label});
    }
    return out;
}

bool IsOfferedCharacter(const std::string& key) {
    for (std::size_t i = 0; i < bodies::OfferedCount(); ++i)
        if (key == bodies::OfferedAt(i).key) return true;
    return false;
}

std::string CharacterKeyOrSam(const std::string& key) {
    // FindByKey ignores case, like the mod reading session.ini; the table's own spelling is kept.
    const bodies::Body* b = bodies::FindByKey(key.c_str());
    return b && b->offered ? std::string(b->key) : std::string(kHostCharacter);
}

std::string CharacterLabel(const std::string& key) {
    const bodies::Body* b = bodies::FindByKey(CharacterKeyOrSam(key).c_str());
    return b ? std::string(b->label) : std::string("Sam");
}

// ---- session.ini ----------------------------------------------------------------
bool MakeSessionIniText(const SessionIni& s, std::string& text, std::string& error) {
    text.clear();
    const bool host = s.role == "host", guest = s.role == "guest", echo = s.role == "echo", solo = s.role == "solo";
    if (!host && !guest && !echo && !solo) {
        error = "role must be host, guest, echo or solo";
        return false;
    }
    if (solo && !s.secret.empty()) {
        error = "a solo (debug tools only) session has no secret";
        return false;
    }
    if (FunctionKeyName(s.debug_menu_key).empty()) {
        error = "debug_menu_key must be F1 to F12";
        return false;
    }
    const bool udp = s.transport == "udp";
    if (!udp && s.transport != "steam") {
        error = "transport must be steam or udp";
        return false;
    }
    if ((host || guest) && !IsCanonicalSecret(s.secret)) {
        error = "the secret must be 20 canonical Crockford base32 characters";
        return false;
    }
    if (echo && !s.secret.empty() && !IsCanonicalSecret(s.secret)) {
        error = "the secret must be 20 canonical Crockford base32 characters";
        return false;
    }
    if (guest && !udp && !IsIndividualSteamId64(s.peer_steamid)) {
        error = "a Steam guest needs the host's SteamID64";
        return false;
    }
    if (!s.peer_steamid.empty() && !IsIndividualSteamId64(s.peer_steamid)) {
        error = "peer_steamid is not an individual SteamID64";
        return false;
    }
    if (udp) {
        if (!IsCanonicalIPv4(s.udp_peer)) {
            error = "udp_peer must be an IPv4 address such as 192.168.1.20";
            return false;
        }
        if (s.udp_port_local < 1 || s.udp_port_local > 65535 || s.udp_port_peer < 1 || s.udp_port_peer > 65535) {
            error = "UDP ports must be 1 to 65535";
            return false;
        }
    }
    if (s.send_hz < 1 || s.send_hz > 120) {
        error = "send_hz must be 1 to 120";
        return false;
    }
    if (s.interp_delay_ms < 0 || s.interp_delay_ms > 1000) {
        error = "interp_delay_ms must be 0 to 1000";
        return false;
    }
    if (!(s.echo_delay_s >= 0.0 && s.echo_delay_s <= kMaxEchoDelayS) || !(s.echo_offset_m >= 0.0 && s.echo_offset_m <= kMaxEchoOffsetM)) {
        error = "echo_delay_s must be 0 to 10 and echo_offset_m 0 to 20";
        return false;
    }
    if (!s.sandbox_saves.empty()) {
        bool name_ok = s.sandbox_saves.size() <= 40;
        for (char c : s.sandbox_saves)
            name_ok = name_ok && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-');
        if (!name_ok) {
            error = "sandbox_saves must be a folder name of letters, digits, '_' and '-'";
            return false;
        }
    } else if (s.sandbox_foreign_saves || s.appdir_settings || s.second_instance) {
        error = "sandbox_foreign_saves, appdir_settings and second_instance need sandbox_saves";
        return false;
    }
    if (!s.local_input.empty() && s.local_input != "host" && s.local_input != "host_pad" && s.local_input != "guest") {
        error = "local_input must be host, host_pad or guest";
        return false;
    }
    if (!s.local_window.empty() && s.local_window != "left" && s.local_window != "right") {
        error = "local_window must be left or right";
        return false;
    }
    if (s.frame_cap != 0 && (s.frame_cap < 15 || s.frame_cap > 240)) {
        error = "frame_cap must be 0 or 15 to 240";
        return false;
    }
    // Only offered bodies: a body nobody has seen work in game is never sent to a partner.
    if (!host && !s.my_character.empty() && !IsOfferedCharacter(s.my_character)) {
        std::string keys;
        for (const CharacterChoice& c : OfferedCharacters()) keys += (keys.empty() ? "" : ", ") + c.key;
        error = "my_character must be one of the offered characters (" + keys + ")";
        return false;
    }
    if (!s.partner_body.empty() && s.partner_body != "real" && s.partner_body != "holo") {
        error = "partner_body must be real or holo";
        return false;
    }
    if (!s.name.empty() && !IsValidPlayerName(s.name)) {
        error = "the name may only use plain letters, digits, spaces and simple punctuation (32 characters at most)";
        return false;
    }
    std::string t;
    t += kHeader;
    t += s.one_shot ? ". The mod renames it to session.last.ini once read; without this file the game runs unmodded.\n"
                    : ". Deleted after the game exits; without this file the game runs unmodded.\n";
    t += "role=" + s.role + "\n";
    t += "transport=" + s.transport + "\n";
    if (!s.peer_steamid.empty()) t += "peer_steamid=" + s.peer_steamid + "\n";
    if (!s.secret.empty()) t += "secret=" + s.secret + "\n";
    if (udp) {
        t += "udp_peer=" + s.udp_peer + "\n";
        t += "udp_port_local=" + std::to_string(s.udp_port_local) + "\n";
        t += "udp_port_peer=" + std::to_string(s.udp_port_peer) + "\n";
    }
    t += "send_hz=" + std::to_string(s.send_hz) + "\n";
    t += "interp_delay_ms=" + std::to_string(s.interp_delay_ms) + "\n";
    t += "echo_delay_s=" + ShortDecimal(s.echo_delay_s) + "\n";
    t += "echo_offset_m=" + ShortDecimal(s.echo_offset_m) + "\n";
    t += std::string("partner_activate=") + (s.partner_activate ? "1" : "0") + "\n";
    t += std::string("verbose=") + (s.verbose ? "1" : "0") + "\n";
    t += std::string("debug_menu=") + (s.debug_menu ? "1" : "0") + "\n";
    t += "debug_menu_key=" + FunctionKeyName(s.debug_menu_key) + "\n";
    t += std::string("one_shot=") + (s.one_shot ? "1" : "0") + "\n";
    if (!s.name.empty()) t += "name=" + s.name + "\n";
    if (!s.sandbox_saves.empty()) {
        t += "sandbox_saves=" + s.sandbox_saves + "\n";
        if (s.sandbox_foreign_saves) t += "sandbox_foreign_saves=1\n";
        if (s.appdir_settings) t += "appdir_settings=1\n";
        if (s.second_instance) t += "second_instance=1\n";
    }
    if (!s.local_input.empty()) t += "local_input=" + s.local_input + "\n";
    if (s.frame_cap) t += "frame_cap=" + std::to_string(s.frame_cap) + "\n";
    if (!s.local_window.empty()) t += "local_window=" + s.local_window + "\n";
    // The host is always Sam (only the guest picks a character). Written for every host, so the
    // rule is plain in the file; the mod applies it too.
    if (host) t += std::string("my_character=") + kHostCharacter + "\n";
    else if (!s.my_character.empty()) t += "my_character=" + s.my_character + "\n";
    if (!s.partner_body.empty()) t += "partner_body=" + s.partner_body + "\n";
    text = std::move(t);
    return true;
}

bool ParseSessionIniText(const std::string& text, SessionIni& out, std::string& error) {
    out = SessionIni{};
    out.transport = "steam";
    out.one_shot = false;   // absent = a persistent (hand-written) file
    for (const auto& kv : ParseIni(text)) {
        const std::string& k = kv.first;
        const std::string& v = kv.second;
        bool ok = true;
        if (k == "role") out.role = v;
        else if (k == "transport") out.transport = v;
        else if (k == "peer_steamid") out.peer_steamid = v;
        else if (k == "secret") out.secret = v;
        else if (k == "udp_peer") out.udp_peer = v;
        else if (k == "udp_port_local") ok = ParseIntStrict(v, 1, 65535, out.udp_port_local);
        else if (k == "udp_port_peer") ok = ParseIntStrict(v, 1, 65535, out.udp_port_peer);
        else if (k == "send_hz") ok = ParseIntStrict(v, 1, 120, out.send_hz);
        else if (k == "interp_delay_ms") ok = ParseIntStrict(v, 0, 1000, out.interp_delay_ms);
        else if (k == "echo_delay_s") ok = ParseDoubleStrict(v, 0.0, kMaxEchoDelayS, out.echo_delay_s);
        else if (k == "echo_offset_m") ok = ParseDoubleStrict(v, 0.0, kMaxEchoOffsetM, out.echo_offset_m);
        else if (k == "verbose") out.verbose = IniBool(v);
        else if (k == "partner_activate") out.partner_activate = IniBool(v);
        else if (k == "debug_menu") out.debug_menu = IniBool(v);
        else if (k == "one_shot") out.one_shot = IniBool(v);
        else if (k == "debug_menu_key") ok = (out.debug_menu_key = ParseFunctionKey(v)) != 0;
        else if (k == "name") out.name = v;
        else if (k == "sandbox_saves") out.sandbox_saves = v;
        else if (k == "sandbox_foreign_saves") out.sandbox_foreign_saves = IniBool(v);
        else if (k == "appdir_settings") out.appdir_settings = IniBool(v);
        else if (k == "second_instance") out.second_instance = IniBool(v);
        else if (k == "local_input") out.local_input = v;
        else if (k == "frame_cap") ok = ParseIntStrict(v, 0, 240, out.frame_cap);
        else if (k == "local_window") out.local_window = v;
        else if (k == "my_character") out.my_character = v;
        else if (k == "partner_body") out.partner_body = v;
        if (!ok) {
            error = "session.ini: " + k + " is malformed";
            return false;
        }
    }
    if (out.role != "host" && out.role != "guest" && out.role != "echo" && out.role != "solo") {
        error = "session.ini: no valid role";
        return false;
    }
    return true;
}

bool IsGeneratedIniText(const std::string& text) {
    std::string t = text;
    if (t.size() >= 3 && static_cast<unsigned char>(t[0]) == 0xEF && static_cast<unsigned char>(t[1]) == 0xBB &&
        static_cast<unsigned char>(t[2]) == 0xBF)
        t = t.substr(3);
    return StartsWith(t, kHeader);
}

std::vector<std::pair<std::string, std::string>> ParseIni(const std::string& text) {
    std::vector<std::pair<std::string, std::string>> out;
    std::string body = text;
    if (body.size() >= 3 && static_cast<unsigned char>(body[0]) == 0xEF && static_cast<unsigned char>(body[1]) == 0xBB &&
        static_cast<unsigned char>(body[2]) == 0xBF)
        body = body.substr(3);
    for (const std::string& raw : SplitLines(body)) {
        const std::string line = Trim(raw);
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos || eq < 1) continue;
        const std::string key = Trim(line.substr(0, eq));
        const std::string value = Trim(line.substr(eq + 1));
        bool replaced = false;
        for (auto& kv : out)
            if (kv.first == key) {
                kv.second = value;
                replaced = true;
            }
        if (!replaced) out.emplace_back(key, value);
    }
    return out;
}

// ---- link-status.json -----------------------------------------------------------
json::Value ParseStatusText(const std::string& text, std::uint32_t expected_pid) {
    json::Value v;
    if (text.empty() || !json::Parse(text, v) || !v.is_object()) return {};
    const json::Value* ver = v.Find("v");
    if (!ver || ver->AsInt(-1) != 1 || !v.Find("state")) return {};
    if (expected_pid) {
        const json::Value* pid = v.Find("pid");
        if (!pid || pid->AsInt(-1) != static_cast<std::int64_t>(expected_pid)) return {};
    }
    return v;
}

bool StatusTimeFresh(const json::Value& status, std::int64_t now_ms) {
    if (!status.is_object()) return false;
    std::int64_t t = 0;
    if (!ParseUtc(status.Str("time_utc"), t)) return false;
    const std::int64_t age = now_ms - t;
    return age <= kStatusStaleMs && age >= -kStatusStaleMs;   // a clock a little ahead still counts
}

StatusView StatusViewOf(const json::Value& status, bool fresh, const std::string& role) {
    StatusView v;
    const bool host = role == "host";
    const bool echo = role == "echo";
    if (!status.is_object()) {
        v.headline = "Waiting for the mod";
        v.detail = "The game is starting. Co-op reports in once the game has loaded.";
        return v;
    }
    v.state = status.Str("state");
    v.transport = status.Str("transport");
    v.route = status.Str("route");
    v.peer = PeerDisplay(status.Str("peer"));
    v.remote = status.Str("remote");
    v.reason = status.Str("reason");
    v.rtt_ms = static_cast<int>(status.Int("rtt_ms", 0));
    v.fresh = fresh;
    if (!fresh) {
        v.headline = "The game is not responding";
        v.detail = "Co-op has not reported for a while. If the game has frozen or closed, this clears by itself once it exits.";
        v.tone = StatusView::Tone::warn;
        v.rtt_ms = 0;
        return v;
    }
    const std::string partner = v.peer.empty() ? std::string("your partner") : v.peer;
    const std::string& s = v.state;
    const std::string because = v.reason.empty() ? std::string() : " (" + v.reason + ")";
    if (role == "solo") {
        v.remote.clear();
        if (s == "error") {
            v.headline = "The debug tools hit an error";
            v.detail = v.reason.empty() ? std::string("See the mod log on the Debug page.") : v.reason;
            v.tone = StatusView::Tone::bad;
        } else {
            v.headline = "Debug tools running";
            v.detail = "No partner and no network: the in-game debug menu and the mod log are on.";
            v.tone = StatusView::Tone::good;
            v.connected = true;
        }
        return v;
    }
    if (echo) {
        if (s == "error") {
            v.headline = "The solo test hit an error";
            v.detail = v.reason.empty() ? std::string("Collect logs from Tools and report it.") : v.reason;
            v.tone = StatusView::Tone::bad;
        } else if (v.remote == "visible") {
            v.headline = "Solo test running";
            v.detail = "Your echo follows a few seconds behind you. Walk around and watch it replay your steps.";
            v.tone = StatusView::Tone::good;
            v.connected = true;
        } else if (v.remote == "failed") {
            v.headline = "The echo could not appear";
            v.detail = "The mod is running, but the copy of your character could not be shown." + because;
            v.tone = StatusView::Tone::bad;
        } else if (v.remote == "spawning") {
            v.headline = "Solo test starting";
            v.detail = "Your echo is appearing next to you...";
        } else {
            v.headline = "Solo test starting";
            v.detail = "Load your save. Your echo appears next to you once you are in the world.";
        }
        return v;
    }
    if (s == "connected") {
        v.connected = true;
        v.tone = StatusView::Tone::good;
        v.headline = "Connected to " + partner;
        if (v.remote == "visible") {
            v.detail = "You can see each other. Keep on keeping on.";
        } else if (v.remote == "spawning") {
            v.detail = "Your partner's porter is appearing in your world...";
        } else if (v.remote == "failed") {
            v.detail = "Connected, but your partner's character could not be shown." + because;
            v.tone = StatusView::Tone::warn;
        } else {
            v.detail = "Your partner appears once you are both in the world.";
        }
    } else if (s == "listening" || s == "waiting" || s == "idle") {
        v.headline = host ? "Waiting for your partner" : "Waiting for the host";
        v.detail = host ? "Send your partner the join code. They paste it on their Join tab."
                        : "The host's game is not reachable yet. It connects once they are in the game.";
    } else if (s == "connecting") {
        v.headline = "Connecting to " + partner;
        v.detail = v.transport == "udp" ? "Connecting directly over your network."
                                        : "Connecting through Steam. This usually takes a few seconds.";
    } else if (s == "handshake") {
        v.headline = "Securing the connection";
        v.detail = "Checking that you both use the same code and the same mod version.";
    } else if (s == "reconnecting") {
        v.headline = "Reconnecting to " + partner;
        v.detail = "The connection dropped for a moment. Co-op tries again by itself." + because;
        v.tone = StatusView::Tone::warn;
    } else if (s == "lost") {
        v.headline = "Lost the connection";
        v.detail = (v.reason.empty() ? std::string("Your partner left or their connection dropped.") : v.reason + ".") +
                   (host ? " They can join again with the same code." : " Ask the host whether they are still in the game.");
        v.tone = StatusView::Tone::warn;
    } else if (s == "rejected") {
        v.headline = "The connection was refused";
        v.detail = v.reason.empty() ? std::string("Check that you both use the same code and the same version of Stranded Together.")
                                    : v.reason;
        v.tone = StatusView::Tone::bad;
    } else if (s == "error") {
        v.headline = "Co-op hit an error";
        v.detail = v.reason.empty() ? std::string("Collect logs from Tools and report it.") : v.reason;
        v.tone = StatusView::Tone::bad;
    } else if ((s == "off" || s == "disabled") && !v.reason.empty()) {
        // The mod's link never started (no valid secret, no host SteamID, a bind failure...).
        v.headline = "Co-op did not start";
        v.detail = v.reason + ". Collect logs from Tools and report it.";
        v.tone = StatusView::Tone::bad;
    } else if (s == "off" || s == "disabled") {
        v.headline = "Co-op is starting";
        v.detail = "The mod is loaded and is starting its connection.";
    } else {
        v.headline = "Co-op is starting";
        v.detail = "The mod reported a state this launcher does not know (" + s + ").";
    }
    return v;
}

std::string PeerDisplay(const std::string& peer) {
    static const char kSteam[] = "steam:";
    if (peer.compare(0, sizeof(kSteam) - 1, kSteam) != 0) return peer;
    const std::string id = peer.substr(sizeof(kSteam) - 1);
    std::string digits;
    for (char c : id)
        if (IsDigit(c)) digits.push_back(c);
    if (digits.size() < 4) return "a Steam player";
    return "Steam player ..." + digits.substr(digits.size() - 4);
}

// ---- Redaction (adapted from Diamond Dogs Together) -------------------------------
namespace {
bool CanonicalIPv4(const std::string& text, std::uint32_t& value) {
    std::uint32_t v = 0;
    int parts = 0;
    std::size_t i = 0;
    while (parts < 4) {
        if (i >= text.size() || !IsDigit(text[i])) return false;
        std::size_t start = i;
        unsigned octet = 0;
        while (i < text.size() && IsDigit(text[i])) {
            octet = octet * 10 + static_cast<unsigned>(text[i] - '0');
            if (i - start >= 3) return false;
            ++i;
        }
        if (i - start > 1 && text[start] == '0') return false;
        if (octet > 255) return false;
        v = (v << 8) | octet;
        ++parts;
        if (parts < 4) {
            if (i >= text.size() || text[i] != '.') return false;
            ++i;
        }
    }
    if (i != text.size()) return false;
    value = v;
    return true;
}

bool InRange(std::uint32_t v, std::uint32_t net, int bits) {
    const std::uint32_t mask = bits == 0 ? 0 : 0xFFFFFFFFu << (32 - bits);
    return (v & mask) == (net & mask);
}

bool ShownAddress(const std::string& ip) {
    std::uint32_t v = 0;
    if (!CanonicalIPv4(ip, v)) return false;
    return InRange(v, 0x7F000000u, 8) || InRange(v, 0x0A000000u, 8) || InRange(v, 0xAC100000u, 12) || InRange(v, 0xC0A80000u, 16) ||
           InRange(v, 0x64400000u, 10) || InRange(v, 0x1A000000u, 8);
}

// [0-9]{1,3}(\.[0-9]{1,3}){3} at `i`, not followed by [0-9.]; length in `n`.
bool MatchIp(const std::string& s, std::size_t i, std::size_t& n) {
    std::size_t p = i;
    for (int part = 0; part < 4; ++part) {
        std::size_t start = p;
        while (p < s.size() && IsDigit(s[p]) && p - start < 3) ++p;
        if (p == start) return false;
        if (part < 3) {
            if (p >= s.size() || s[p] != '.') return false;
            ++p;
        }
    }
    if (p < s.size() && (IsDigit(s[p]) || s[p] == '.')) return false;
    n = p - i;
    return true;
}

bool IStartsAt(const std::string& s, std::size_t i, const char* word) {
    const std::size_t n = std::strlen(word);
    if (i + n > s.size()) return false;
    for (std::size_t k = 0; k < n; ++k) {
        char a = s[i + k], b = word[k];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}
}  // namespace

std::string MaskAddress(const std::string& address) {
    if (address.empty()) return address;
    // ^a.b.c.d(:port)?$
    std::size_t colon = address.find(':');
    const std::string ip = colon == std::string::npos ? address : address.substr(0, colon);
    const std::string rest = colon == std::string::npos ? std::string() : address.substr(colon);
    if (!rest.empty()) {
        if (rest.size() < 2) return address;
        for (std::size_t i = 1; i < rest.size(); ++i)
            if (!IsDigit(rest[i])) return address;
    }
    std::size_t dots = 0;
    for (char c : ip) {
        if (c == '.') ++dots;
        else if (!IsDigit(c)) return address;
    }
    if (dots != 3) return address;
    const std::size_t d1 = ip.find('.'), d2 = ip.find('.', d1 + 1);
    if (d1 == 0 || d2 == d1 + 1) return address;
    if (ShownAddress(ip)) return address;
    return ip.substr(0, d2) + ".x.x" + rest;
}

std::string RedactSensitive(const std::string& text) {
    if (text.empty()) return text;
    std::string out;
    out.reserve(text.size());
    // 1. 17-digit runs: SteamIDs keep their first 7 and last 4 digits.
    for (std::size_t i = 0; i < text.size();) {
        if (IsDigit(text[i]) && (i == 0 || !IsDigit(text[i - 1]))) {
            std::size_t j = i;
            while (j < text.size() && IsDigit(text[j])) ++j;
            const std::string run = text.substr(i, j - i);
            if (run.size() == 17 && (StartsWith(run, "7656119") || IsIndividualSteamId64(run)))
                out += run.substr(0, 7) + "XXXXXX" + run.substr(13);
            else
                out += run;
            i = j;
            continue;
        }
        out.push_back(text[i++]);
    }
    // 2. userdata\<account>.
    std::string step2;
    for (std::size_t i = 0; i < out.size();) {
        if (IStartsAt(out, i, "userdata")) {
            std::size_t j = i + 8;
            std::size_t seps = j;
            while (seps < out.size() && (out[seps] == '\\' || out[seps] == '/')) ++seps;
            std::size_t digits = seps;
            while (digits < out.size() && IsDigit(out[digits])) ++digits;
            if (seps > j && digits > seps) {
                step2 += out.substr(i, seps - i) + "<account>";
                i = digits;
                continue;
            }
        }
        step2.push_back(out[i++]);
    }
    // 3. X:\Users\<user>.
    std::string step3;
    for (std::size_t i = 0; i < step2.size();) {
        const char c = step2[i];
        const bool letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        if (letter && (i == 0 || !IsWordChar(step2[i - 1])) && i + 1 < step2.size() && step2[i + 1] == ':') {
            std::size_t p = i + 2;
            std::size_t s1 = p;
            while (p < step2.size() && (step2[p] == '\\' || step2[p] == '/')) ++p;
            if (p > s1 && IStartsAt(step2, p, "users")) {
                std::size_t q = p + 5;
                std::size_t s2 = q;
                while (q < step2.size() && (step2[q] == '\\' || step2[q] == '/')) ++q;
                std::size_t name = q;
                while (name < step2.size() && std::strchr("\\/\r\n\"';", step2[name]) == nullptr) ++name;
                if (q > s2 && name > q) {
                    step3 += step2.substr(i, q - i) + "<user>";
                    i = name;
                    continue;
                }
            }
        }
        step3.push_back(step2[i++]);
    }
    return step3;
}


std::string RedactNetwork(const std::string& text) {
    if (text.empty()) return text;
    // Join codes: DST1, blanks or dashes, 22 code characters, optional @host.
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        if (IStartsAt(text, i, "DST1")) {
            std::size_t p = i + 4;
            while (p < text.size() && (IsBlank(text[p]) || text[p] == '-')) ++p;
            int count = 0;
            std::size_t last_end = p;
            while (p < text.size() && count < 22) {
                if (!IsAlnumAscii(text[p])) break;
                ++count;
                ++p;
                last_end = p;
                if (count < 22)
                    while (p < text.size() && (IsBlank(text[p]) || text[p] == '-')) ++p;
            }
            if (count == 22) {
                std::size_t end = last_end;
                std::size_t q = end;
                while (q < text.size() && (IsBlank(text[q]) || text[q] == '-')) ++q;
                if (q + 1 < text.size() && text[q] == '@' && !IsBlank(text[q + 1])) {
                    q += 1;
                    while (q < text.size() && !IsBlank(text[q]) && text[q] != '"') ++q;
                    end = q;
                }
                out += "DST1-<redacted>";
                i = end;
                continue;
            }
        }
        out.push_back(text[i++]);
    }
    // Invite strings and secret= values.
    std::string step2;
    static const char kJoin[] = "+stranded_join";
    static const char kSecret[] = "secret=";
    for (std::size_t i = 0; i < out.size();) {
        if (out.compare(i, sizeof(kJoin) - 1, kJoin) == 0) {
            std::size_t p = i + sizeof(kJoin) - 1;
            const std::size_t blanks = p;
            while (p < out.size() && IsBlank(out[p])) ++p;
            std::size_t tok = p;
            while (tok < out.size() && !IsBlank(out[tok])) ++tok;
            if (p > blanks && tok > p) {
                step2 += "+stranded_join <redacted>";
                i = tok;
                continue;
            }
        }
        if ((i == 0 || !IsWordChar(out[i - 1])) && out.compare(i, sizeof(kSecret) - 1, kSecret) == 0) {
            const std::size_t p = i + sizeof(kSecret) - 1;
            std::size_t tok = p;
            while (tok < out.size() && !IsBlank(out[tok]) && out[tok] != ';' && out[tok] != '"') ++tok;
            if (tok > p) {
                step2 += "secret=<redacted>";
                i = tok;
                continue;
            }
        }
        step2.push_back(out[i++]);
    }
    // Public IPv4 addresses after peer=, udp_peer=, host= and "peer":".
    std::string step3;
    for (std::size_t i = 0; i < step2.size();) {
        std::size_t prefix = 0;
        const bool boundary = i == 0 || !IsWordChar(step2[i - 1]);
        if (boundary && step2.compare(i, 5, "peer=") == 0) prefix = 5;
        else if (boundary && step2.compare(i, 9, "udp_peer=") == 0) prefix = 9;
        else if (boundary && step2.compare(i, 5, "host=") == 0) prefix = 5;
        else if (step2.compare(i, 6, "\"peer\"") == 0) {
            std::size_t p = i + 6;
            while (p < step2.size() && IsBlank(step2[p])) ++p;
            if (p < step2.size() && step2[p] == ':') {
                ++p;
                while (p < step2.size() && IsBlank(step2[p])) ++p;
                if (p < step2.size() && step2[p] == '"') prefix = p + 1 - i;
            }
        }
        std::size_t n = 0;
        if (prefix && MatchIp(step2, i + prefix, n)) {
            step3 += step2.substr(i, prefix) + MaskAddress(step2.substr(i + prefix, n));
            i += prefix + n;
            continue;
        }
        step3.push_back(step2[i++]);
    }
    return step3;
}

std::string Redact(const std::string& text) { return RedactNetwork(RedactSensitive(text)); }

std::string RedactForLog(const std::string& text) {
    const std::string r = Redact(text);
    std::string out;
    std::size_t i = 0;
    while (i < r.size()) {
        if (IsBlank(r[i])) {
            out.push_back(r[i++]);
            continue;
        }
        std::size_t j = i;
        while (j < r.size() && !IsBlank(r[j])) ++j;
        const std::string token = r.substr(i, j - i);
        out += token.find('\\') != std::string::npos || token.find(":/") != std::string::npos ? "<path>" : token;
        i = j;
    }
    return out;
}

}  // namespace st::core
