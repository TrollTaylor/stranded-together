// STT1: Fragile's edited textures over the link (docs/textures.md, docs/real-partner-body.md section 29), on the
// reliable ordered channel 5 ("assets"). Pure: no Windows, no game. Little endian, strict decoders, every message at
// most 1024 bytes (the reliable channel's limit).
//
//   "STT1" u8 version (1) u8 kind, then the body:
//   1 LOOK   guest -> host  u32 epoch, u32 table (tex_slots kTableVersion), u8 flags (bit0 boots own, bit1 tint fallback, bit2 logo),
//                           u8 suit colour, u8 boot type, u8 worn, u8 n (0..8), n x {u8 slot, 32-byte blob id, u32 size}.
//                           n = 0: her own look.
//   2 HAVE   host -> guest  u32 epoch, u8 n (0..8), n x 32-byte blob id (cached and valid on the host)
//   3 WANT   host -> guest  u32 epoch, u8 n (0..8), n x {32-byte blob id, u32 resume offset}
//   4 CHUNK  guest -> host  8-byte blob id prefix, u32 offset, u16 len (1..980), the bytes
//   5 RESULT host -> guest  32-byte blob id, u8 code (Code)
//   6 CANCEL either         u32 epoch
//   7 RECIPE guest -> host  u32 epoch (the LOOK that follows), u8 generator, u16 blob encoder, 32-byte originals H,
//                           u8 n (1..8), n x {u8 slot, u8 colour (1..16), u8 variant, 32-byte blob id}
//                           (round 8): these blobs of the LOOK are Stranded Together's ready-made colour, made from her
//                           Original pictures whose identity is H (suit_defaults.hpp OriginalsId). A host with the same
//                           pictures makes them itself and checks the SHA-256; otherwise it asks for the blob (WANT).
//                           Sent just before its LOOK, in the same tick; an older decoder ignores the kind.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace st::twire {

inline constexpr std::uint8_t kVersion = 1;
inline constexpr std::size_t kMaxMessage = 1024;
inline constexpr std::size_t kChunkData = 980;
inline constexpr unsigned kMaxEntries = 8;
inline constexpr std::uint32_t kMaxBlob = 6u * 1024u * 1024u;

enum class Kind : std::uint8_t { none = 0, look = 1, have = 2, want = 3, chunk = 4, result = 5, cancel = 6, recipe = 7 };
enum Code : std::uint8_t { kOk = 0, kHashMismatch = 1, kInvalid = 2, kTooLarge = 3, kRefused = 4, kUnknownSlot = 5, kBudget = 6 };
const char* CodeName(std::uint8_t code);

struct LookEntry {
    std::uint8_t slot = 0;
    std::uint8_t id[32] = {};
    std::uint32_t size = 0;
};
struct Look {
    std::uint32_t epoch = 0;
    std::uint32_t table = 0;
    std::uint8_t flags = 0, colour = 0, boots = 0, worn = 0;
    std::uint8_t n = 0;
    LookEntry e[kMaxEntries];
};
struct IdList {  // HAVE (offsets unused) and WANT
    std::uint32_t epoch = 0;
    std::uint8_t n = 0;
    std::uint8_t id[kMaxEntries][32] = {};
    std::uint32_t offset[kMaxEntries] = {};
};
struct Chunk {
    std::uint8_t prefix[8] = {};
    std::uint32_t offset = 0;
    std::uint16_t len = 0;
    const std::uint8_t* data = nullptr;  // decode: points into the message
};
struct Result {
    std::uint8_t id[32] = {};
    std::uint8_t code = 0;
};
inline constexpr unsigned kMetalVariants = 4;   // suit_defaults.hpp kMetalVariants (a RECIPE's variant is below it; static_assert in texture_share.cpp)
struct RecipeEntry {
    std::uint8_t slot = 0, colour = 0, variant = 0;
    std::uint8_t id[32] = {};
};
struct Recipe {
    std::uint32_t epoch = 0;
    std::uint8_t generator = 0;
    std::uint16_t encoder = 0;
    std::uint8_t originals[32] = {};
    std::uint8_t n = 0;
    RecipeEntry e[kMaxEntries];
};

Kind PeekKind(const std::uint8_t* p, std::size_t n);  // none for anything that is not a well-formed STT1 header
// Encoders return the message size (0 when it does not fit `cap` or a field is out of range).
std::size_t EncodeLook(const Look& m, std::uint8_t* out, std::size_t cap);
std::size_t EncodeIds(Kind kind, const IdList& m, std::uint8_t* out, std::size_t cap);  // have / want
std::size_t EncodeChunk(const Chunk& m, std::uint8_t* out, std::size_t cap);
std::size_t EncodeResult(const Result& m, std::uint8_t* out, std::size_t cap);
std::size_t EncodeCancel(std::uint32_t epoch, std::uint8_t* out, std::size_t cap);
std::size_t EncodeRecipe(const Recipe& m, std::uint8_t* out, std::size_t cap);
// Strict decoders: the exact length, every count and length in range.
bool DecodeLook(const std::uint8_t* p, std::size_t n, Look& out);
bool DecodeIds(const std::uint8_t* p, std::size_t n, Kind kind, IdList& out);
bool DecodeChunk(const std::uint8_t* p, std::size_t n, Chunk& out);
bool DecodeResult(const std::uint8_t* p, std::size_t n, Result& out);
bool DecodeCancel(const std::uint8_t* p, std::size_t n, std::uint32_t& epoch);
// Strict: n 1..8, colour 1..16, variant below kMetalVariants, no slot twice, generator not 0.
bool DecodeRecipe(const std::uint8_t* p, std::size_t n, Recipe& out);

// A blob being received: fixed 980-byte pieces (the last one shorter), in any order, each once. Out-of-range, misaligned
// or wrongly sized pieces are refused; duplicates are ignored.
class Assembly {
public:
    bool Init(std::uint32_t size);          // false for 0 or more than kMaxBlob
    // true when the piece was taken (new), false when refused or a duplicate.
    bool Add(std::uint32_t offset, const std::uint8_t* data, std::size_t len);
    bool Complete() const { return have_ == pieces_.size() && size_ > 0; }
    std::uint32_t ResumeOffset() const;     // the first missing piece's offset (size when complete)
    std::uint32_t size() const { return size_; }
    std::uint32_t received() const { return received_; }
    const std::vector<std::uint8_t>& bytes() const { return bytes_; }
    std::vector<std::uint8_t> Take();

private:
    std::uint32_t size_ = 0, received_ = 0;
    std::size_t have_ = 0;
    std::vector<std::uint8_t> bytes_;
    std::vector<std::uint8_t> pieces_;
};

// The sender's pacing: a token bucket of `rate` bytes per second, at most `burst` bytes saved up.
class Bucket {
public:
    void Reset(double now_ms) { last_ = now_ms, tokens_ = 0; }
    // Adds what `rate` gave since the last call; true when `bytes` may be sent now (and takes them).
    bool Take(double now_ms, double rate_bytes_per_s, double burst, std::size_t bytes);

private:
    double last_ = -1, tokens_ = 0;
};

}  // namespace st::twire
