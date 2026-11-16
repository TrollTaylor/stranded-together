// Stranded Together: join-code check for the Join tab (see launcher_join_code.hpp).
#include "launcher_join_code.hpp"

#include <cstring>

#include "stcoop/link_crypto.hpp"
#include "stcoop/steam_join_code.hpp"

namespace st {
namespace {
namespace code_ns = stcoop::steam_code;
namespace link_ns = stcoop::link;

bool IsBlank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }

// Strips blanks and an optional "+stranded_join" invite prefix.
std::string Compact(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text)
        if (!IsBlank(c)) out.push_back(c);
    static const char kInvite[] = "+stranded_join";
    const std::size_t n = sizeof(kInvite) - 1;
    if (out.size() >= n && _strnicmp(out.c_str(), kInvite, n) == 0) out.erase(0, n);
    return out;
}
}  // namespace

CodeCheck CheckJoinCode(const std::string& text, std::uint64_t own_steam_id) {
    const std::string compact = Compact(text);
    if (compact.empty()) return {false, ""};
    if (compact.size() > 200) return {false, "That is too long to be a join code. Copy just the code."};
    code_ns::Code code{};
    const auto status = code_ns::Parse(compact.data(), compact.size(), code);
    using code_ns::Status;
    switch (status) {
    case Status::ok: break;
    case Status::empty: return {false, ""};
    case Status::bad_prefix: return {false, "That doesn't look like a join code. A join code starts with DST1-."};
    case Status::bad_char: return {false, "The code has a character that can't be in a join code. Copy it again."};
    case Status::bad_length: return {false, "Part of the code is missing. Copy all of it, including the part after @."};
    case Status::bad_check: return {false, "The code has a typo. Copy it again, or ask your partner to send it again."};
    case Status::bad_steamid:
    default:
        if (compact.find('@') == std::string::npos) return {false, "Paste the whole code, including the part after @."};
        return {false, "The part after @ is not a Steam account. Copy the whole code again."};
    }
    const bool own = own_steam_id != 0 && code.host == own_steam_id;
    link_ns::SecureZero(&code, sizeof(code));
    if (own) return {false, "That is your own code. Your partner enters it, not you."};
    return {true, "The code looks right."};
}

bool ParseJoinCode(const std::string& text, std::string& secret_out, std::uint64_t& host_out) {
    secret_out.clear();
    host_out = 0;
    const std::string compact = Compact(text);
    if (compact.empty() || compact.size() > 200) return false;
    code_ns::Code code{};
    if (code_ns::Parse(compact.data(), compact.size(), code) != code_ns::Status::ok) return false;
    secret_out.assign(code.secret, link_ns::kSecretChars);
    host_out = code.host;
    link_ns::SecureZero(&code, sizeof(code));
    return true;
}

std::string FormatJoinCode(const std::string& secret20, std::uint64_t host_steam_id) {
    if (secret20.size() != link_ns::kSecretChars) return {};
    char secret[link_ns::kSecretChars];
    std::memcpy(secret, secret20.data(), sizeof(secret));
    char out[code_ns::kCodeBuffer]{};
    const bool ok = code_ns::Format(secret, host_steam_id, out, sizeof(out));
    link_ns::SecureZero(secret, sizeof(secret));
    return ok ? std::string(out) : std::string();
}

}  // namespace st
