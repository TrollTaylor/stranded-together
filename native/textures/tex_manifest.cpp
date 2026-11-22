// Stranded Together textures: the manifest (tex_manifest.hpp).
#include "tex_manifest.hpp"

#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace st::tex {
namespace {

std::vector<std::string> SplitTabs(const std::string& s) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (;;) {
        const std::size_t t = s.find('\t', start);
        out.push_back(s.substr(start, t == std::string::npos ? std::string::npos : t - start));
        if (t == std::string::npos) break;
        start = t + 1;
    }
    return out;
}

bool IsHex(const std::string& s, std::size_t n) {
    if (s.size() != n) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

char Fold(char c) {
    if (c == '/') return '\\';
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

}  // namespace

bool SamePath(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (Fold(a[i]) != Fold(b[i])) return false;
    return true;
}

const ManifestRow* Manifest::Find(const std::string& path) const {
    for (const ManifestRow& r : rows)
        if (SamePath(r.path, path)) return &r;
    return nullptr;
}

std::string WriteManifest(const Manifest& m) {
    std::string s = "stx-manifest " + std::to_string(kManifestVersion) + "\n";
    char table[16];
    std::snprintf(table, sizeof(table), "%08x", m.table);
    s += std::string("table ") + table + "\tgame " + m.game + "\ttool " + m.tool + "\textract " + std::to_string(m.extract) + "\n";
    for (const ManifestRow& r : m.rows) {
        s += "file " + r.path + "\t" + r.slot + "\t" + r.component + "\t" + std::to_string(r.w) + "x" + std::to_string(r.h) + "\t" + r.sha256 +
             "\t" + r.kind + "\t" + std::to_string(r.bytes) + "\n";
    }
    return s;
}

bool ParseManifest(const std::string& text, Manifest& out, std::string& error) {
    out = Manifest{};
    std::istringstream in(text);
    std::string line;
    int n = 0;
    bool header = false, table = false;
    while (std::getline(in, line)) {
        ++n;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const std::string where = "line " + std::to_string(n);
        if (!header) {
            if (line != "stx-manifest " + std::to_string(kManifestVersion)) return error = where + ": not a version 1 manifest", false;
            header = true;
            continue;
        }
        if (line.rfind("table ", 0) == 0) {
            const std::vector<std::string> f = SplitTabs(line);
            if ((f.size() != 3 && f.size() != 4) || f[1].rfind("game ", 0) != 0 || f[2].rfind("tool ", 0) != 0 || (f.size() == 4 && f[3].rfind("extract ", 0) != 0))
                return error = where + ": bad table line", false;
            const std::string hex = f[0].substr(6);
            if (!IsHex(hex, 8)) return error = where + ": bad table value", false;
            out.table = static_cast<std::uint32_t>(std::strtoul(hex.c_str(), nullptr, 16));
            out.game = f[1].substr(5);
            out.tool = f[2].substr(5);
            if (f.size() == 4) {
                char* end = nullptr;
                const long v = std::strtol(f[3].c_str() + 8, &end, 10);
                if (f[3].size() == 8 || !end || *end || v < 0 || v > 1000000) return error = where + ": bad extract value", false;
                out.extract = static_cast<int>(v);
            }
            table = true;
            continue;
        }
        if (line.rfind("file ", 0) != 0) return error = where + ": unknown line", false;
        const std::vector<std::string> f = SplitTabs(line.substr(5));
        if (f.size() != 7) return error = where + ": a file line needs 7 fields", false;
        ManifestRow r;
        r.path = f[0];
        r.slot = f[1];
        r.component = f[2];
        const std::size_t x = f[3].find('x');
        if (r.path.empty() || r.path.find("..") != std::string::npos || r.path[0] == '\\' || r.path.find(':') != std::string::npos)
            return error = where + ": bad path", false;
        if (x == std::string::npos) return error = where + ": bad size", false;
        r.w = static_cast<std::uint32_t>(std::strtoul(f[3].substr(0, x).c_str(), nullptr, 10));
        r.h = static_cast<std::uint32_t>(std::strtoul(f[3].substr(x + 1).c_str(), nullptr, 10));
        if (r.w == 0 || r.h == 0 || r.w > 16384 || r.h > 16384) return error = where + ": bad size", false;
        if (!IsHex(f[4], 64)) return error = where + ": bad sha256", false;
        r.sha256 = f[4];
        r.kind = f[5];
        if (r.kind != "original" && r.kind != "seed" && r.kind != "ref" && r.kind != "preview") return error = where + ": unknown kind", false;
        char* end = nullptr;
        r.bytes = std::strtoull(f[6].c_str(), &end, 10);
        if (f[6].empty() || !end || *end) return error = where + ": bad byte count", false;
        out.rows.push_back(std::move(r));
    }
    if (!header || !table) return error = "the manifest is incomplete", false;
    return true;
}

int FolderNumber(const std::string& name) {
    if (name.size() < 3 || name[0] < '0' || name[0] > '9' || name[1] < '0' || name[1] > '9' || name[2] != ' ') return -1;
    return (name[0] - '0') * 10 + (name[1] - '0');
}

}  // namespace st::tex
