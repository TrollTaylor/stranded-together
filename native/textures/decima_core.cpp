// Stranded Together textures: Texture / TextureSet core objects (decima_core.hpp).
#include "decima_core.hpp"

#include <cstdio>
#include <cstring>

#include "tex_codec.hpp"

namespace st::tex {
namespace {

struct Cursor {
    const std::uint8_t* p;
    std::uint32_t size;
    std::uint32_t pos = 0;
    bool ok = true;
    bool Need(std::uint32_t n) {
        if (!ok || n > size - pos) ok = false;
        return ok;
    }
    template <typename T>
    T Get() {
        T v{};
        if (Need(sizeof(T))) {
            std::memcpy(&v, p + pos, sizeof(T));
            pos += sizeof(T);
        }
        return v;
    }
    const std::uint8_t* Bytes(std::uint32_t n) {
        if (!Need(n)) return nullptr;
        const std::uint8_t* r = p + pos;
        pos += n;
        return r;
    }
    // Decima's hashed String: u32 length; length > 0: u32 crc32c, then the bytes.
    std::string HString() {
        const std::uint32_t n = Get<std::uint32_t>();
        if (!ok || n == 0) return {};
        Get<std::uint32_t>();
        if (n > 4096) return ok = false, std::string();
        const std::uint8_t* b = Bytes(n);
        return b ? std::string(reinterpret_cast<const char*>(b), n) : std::string();
    }
};

}  // namespace

std::string GuidText(const std::uint8_t* g) {
    char buf[40];
    std::uint32_t a;
    std::uint16_t b, c;
    std::memcpy(&a, g, 4);
    std::memcpy(&b, g + 4, 2);
    std::memcpy(&c, g + 6, 2);
    std::snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", a, b, c, g[8], g[9], g[10], g[11], g[12], g[13], g[14],
                  g[15]);
    return buf;
}

bool ReadCoreObjects(const std::vector<std::uint8_t>& core, std::vector<CoreObject>& out, std::string& error) {
    out.clear();
    std::size_t pos = 0;
    while (pos + 12 <= core.size()) {
        CoreObject o;
        std::memcpy(&o.type, core.data() + pos, 8);
        std::memcpy(&o.size, core.data() + pos + 8, 4);
        if (o.size > core.size() - pos - 12) return error = "a core object runs past the end of the file", false;
        o.body = core.data() + pos + 12;
        out.push_back(o);
        pos += 12 + static_cast<std::size_t>(o.size);
    }
    if (pos != core.size()) return error = "the core file does not end on an object boundary", false;
    return true;
}

bool ParseTexture(const std::uint8_t* body, std::uint32_t size, TextureInfo& t, std::string& error) {
    t = TextureInfo{};
    if (size < 72) return error = "texture too short", false;
    t.guid = GuidText(body);
    const std::uint8_t* h = body + 16;
    t.kind = h[0] & 0x0Fu;
    std::uint16_t w, hh;
    std::memcpy(&w, h + 2, 2);
    std::memcpy(&hh, h + 4, 2);
    t.width = w & 0x3FFFu;
    t.height = hh & 0x3FFFu;
    t.mips = h[8];
    t.format = h[9];
    t.srgb = h[11];
    t.flags = h[13];
    t.content_guid = GuidText(h + 16);
    Cursor c{body, size, 48};
    const std::uint32_t remaining = c.Get<std::uint32_t>();
    if (remaining != size - 52) return error = "texture 'remaining' does not match its size", false;
    t.embedded_size = c.Get<std::uint32_t>();
    t.stream_size = c.Get<std::uint32_t>();
    t.stream_mips = c.Get<std::uint32_t>();
    t.views = c.Get<std::uint64_t>();
    if (t.stream_size) {
        const std::uint32_t n = c.Get<std::uint32_t>();
        if (n == 0 || n > 1024) return error = "bad stream path", false;
        const std::uint8_t* p = c.Bytes(n);
        if (!p) return error = "texture truncated (stream path)", false;
        t.stream_path.assign(reinterpret_cast<const char*>(p), n);
        const std::uint8_t* g = c.Bytes(16);
        if (!g) return error = "texture truncated (stream guid)", false;
        c.Get<std::uint32_t>();   // channel
        t.stream_offset = static_cast<std::uint32_t>(c.Get<std::int32_t>());
        t.stream_length = static_cast<std::uint32_t>(c.Get<std::int32_t>());
        if (t.stream_length != t.stream_size) return error = "stream length differs from the stream size", false;
    }
    const std::uint8_t* e = c.Bytes(t.embedded_size);
    if (!c.ok || !e) return error = "texture truncated (embedded data)", false;
    if (c.pos != size) return error = "texture has trailing bytes", false;
    t.embedded.assign(e, e + t.embedded_size);
    return true;
}

bool ParseTextureSet(const std::uint8_t* body, std::uint32_t size, TextureSetInfo& s, std::string& error) {
    s = TextureSetInfo{};
    Cursor c{body, size, 0};
    const std::uint8_t* g = c.Bytes(16);
    if (!g) return error = "texture set too short", false;
    s.guid = GuidText(g);
    const std::uint32_t n = c.Get<std::uint32_t>();
    if (n > 64) return error = "too many texture set entries", false;
    for (std::uint32_t i = 0; c.ok && i < n; ++i) {
        TextureSetEntry e;
        e.compress = c.Get<std::uint32_t>();
        e.create_mips = c.Get<std::uint8_t>();
        e.colour_space = c.Get<std::uint32_t>();
        e.packing = c.Get<std::uint32_t>();
        e.texture_type = c.Get<std::int32_t>();
        e.ref_kind = c.Get<std::uint8_t>();
        if (e.ref_kind > 0) {
            const std::uint8_t* rg = c.Bytes(16);
            if (rg) e.texture_guid = GuidText(rg);
            if (e.ref_kind >= 2) e.texture_path = c.HString();
        }
        s.entries.push_back(e);
    }
    c.Get<std::uint32_t>();   // MipMapAddressMode
    c.Get<std::int32_t>();    // OverrideCompressionLambda
    const std::uint32_t nd = c.Get<std::uint32_t>();
    if (nd > 64) return error = "too many texture set descriptions", false;
    for (std::uint32_t i = 0; c.ok && i < nd; ++i) {
        TextureSetDesc d;
        d.type = c.Get<std::uint32_t>();
        d.path = c.HString();
        c.Get<std::uint8_t>();   // Active
        c.Get<std::uint8_t>();   // GammaSpace
        d.storage = c.Get<std::uint32_t>();
        c.Get<std::uint32_t>();   // QualityType
        c.Get<std::uint32_t>();   // CompressionMethod
        d.width = c.Get<std::int32_t>();
        d.height = c.Get<std::int32_t>();
        c.Bytes(16);   // DefaultColor
        s.descs.push_back(d);
    }
    const std::uint8_t kind = c.Get<std::uint8_t>();   // Preset ref
    if (kind > 0) {
        c.Bytes(16);
        if (kind >= 2) c.HString();
    }
    if (!c.ok) return error = "texture set truncated", false;
    if (c.pos != size) return error = "texture set has trailing bytes", false;
    return true;
}

bool CheckTexture(const TextureInfo& t, std::size_t stream_file_size, std::string& error) {
    if (t.kind != 0) return error = "not a 2D texture", false;
    if (t.width == 0 || t.height == 0 || t.width > 4096 || t.height > 4096) return error = "size outside 1..4096", false;
    if (t.mips == 0 || t.mips > FullMipCount(t.width, t.height)) return error = "bad mip count", false;
    if (!CanDecode(t.format) && t.format != kBC6U) return error = std::string("format ") + FormatName(t.format) + " not allowed", false;
    const std::uint64_t chain = ChainBytes(t.format, t.width, t.height, t.mips);
    if (chain != static_cast<std::uint64_t>(t.stream_size) + t.embedded_size)
        return error = "mip sizes do not add up (" + std::to_string(chain) + " vs " + std::to_string(t.stream_size + t.embedded_size) + ")", false;
    if (t.stream_mips > t.mips) return error = "more streamed mips than mips", false;
    if (ChainBytes(t.format, t.width, t.height, t.stream_mips) != t.stream_size) return error = "streamed mips do not match the stream size", false;
    if (t.stream_size && static_cast<std::uint64_t>(t.stream_offset) + t.stream_length > stream_file_size)
        return error = "stream slice outside the .core.stream file", false;
    return true;
}

bool MipData(const TextureInfo& t, const std::vector<std::uint8_t>& stream, std::uint32_t level, const std::uint8_t*& data, std::size_t& size,
             std::string& error) {
    if (level >= t.mips) return error = "no such mip", false;
    std::uint32_t w = t.width, h = t.height;
    std::uint64_t before = 0;
    const std::uint32_t first = level < t.stream_mips ? 0 : t.stream_mips;
    for (std::uint32_t i = 0; i < level; ++i) {
        if (i >= first) before += MipBytes(t.format, w, h);
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    const std::uint64_t n = MipBytes(t.format, w, h);
    if (level < t.stream_mips) {
        if (static_cast<std::uint64_t>(t.stream_offset) + before + n > stream.size()) return error = "stream too short", false;
        data = stream.data() + t.stream_offset + before;
    } else {
        if (before + n > t.embedded.size()) return error = "embedded data too short", false;
        data = t.embedded.data() + before;
    }
    size = static_cast<std::size_t>(n);
    return true;
}

}  // namespace st::tex
