// Stranded Together textures: the two core-object layouts the extractor reads (Texture and TextureSet, Death Stranding
// Director's Cut), hard-coded from the serialized order (no type JSON). Layouts: scratchpad research-archive.md section 2,
// checked on all 31 sets the extractor reads. Every parser checks its sizes and refuses with a reason.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace st::tex {

constexpr std::uint64_t kTypeTexture = 0x3B7A8F4FBDDCD834ull;
constexpr std::uint64_t kTypeTextureSet = 0x00B4A8881829A349ull;

struct CoreObject {
    std::uint64_t type = 0;
    const std::uint8_t* body = nullptr;
    std::uint32_t size = 0;
};
// u64 type, u32 size, body... False when the objects do not end exactly at the end of the data.
bool ReadCoreObjects(const std::vector<std::uint8_t>& core, std::vector<CoreObject>& out, std::string& error);

std::string GuidText(const std::uint8_t* g16);   // "2d99cdfc-7b94-c338-90e2-9d1c9ba236b6" (Windows byte order)

struct TextureInfo {
    std::string guid;               // ObjectUUID
    std::uint8_t kind = 0;          // header +0 low nibble (0 = 2D)
    std::uint32_t width = 0, height = 0;
    std::uint32_t mips = 0;
    std::uint8_t format = 0;        // EPixelFormat
    std::uint8_t srgb = 0;          // header +0x0B
    std::uint8_t flags = 0;         // header +0x0D
    std::string content_guid;       // header +0x10 (the GUID cache key)
    std::uint32_t embedded_size = 0, stream_size = 0, stream_mips = 0;
    std::uint64_t views = 0;        // the u64 after stream_mips
    std::string stream_path;        // without ".core.stream"
    std::uint32_t stream_offset = 0, stream_length = 0;
    std::vector<std::uint8_t> embedded;   // the embedded tail (smaller mips)
};
bool ParseTexture(const std::uint8_t* body, std::uint32_t size, TextureInfo& out, std::string& error);

struct TextureSetEntry {
    std::uint32_t compress = 0;
    std::uint8_t create_mips = 0;
    std::uint32_t colour_space = 0;
    std::uint32_t packing = 0;
    std::int32_t texture_type = 0;
    std::uint8_t ref_kind = 0;
    std::string texture_guid;       // "" when the ref is empty
    std::string texture_path;       // an external ref's core path ("" for a local one)
};
struct TextureSetDesc {
    std::uint32_t type = 0;         // ETextureSetType
    std::string path;               // the source .psd name
    std::uint32_t storage = 0;      // 0 RGB, 1 R, 2 G, 3 B, 4 A
    std::int32_t width = 0, height = 0;
};
struct TextureSetInfo {
    std::string guid;
    std::vector<TextureSetEntry> entries;
    std::vector<TextureSetDesc> descs;
};
bool ParseTextureSet(const std::uint8_t* body, std::uint32_t size, TextureSetInfo& out, std::string& error);

// Mip `level`'s bytes: streamed mips first (largest first) from `stream` (the whole .core.stream file), then the
// embedded tail. False when the sizes do not add up.
bool MipData(const TextureInfo& t, const std::vector<std::uint8_t>& stream, std::uint32_t level, const std::uint8_t*& data,
             std::size_t& size, std::string& error);
// The texture's own checks: tight chain sum == stream + embedded, stream slice inside the file, dims, format.
bool CheckTexture(const TextureInfo& t, std::size_t stream_file_size, std::string& error);

}  // namespace st::tex
