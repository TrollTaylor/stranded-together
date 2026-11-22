// Stranded Together textures: a read-only reader for the game's Decima packfiles (<game>\data\*.bin), ported from
// tools/re/ds_archive.py (its docstring has the format). Only for the launcher's "Extract textures" child process and
// the tests: never linked into the mod.
//   Header (40 bytes): u32 magic (0x20304050 plain, 0x21304050 encrypted), u32 key, then 32 bytes (XOR-encrypted
//     with the masks of key and key+1): u64 file_size, u64 data_size, u64 entry_count, u32 chunk_count, u32 max_chunk.
//   Entry table: entry_count x 32 bytes {u32 index, u32 key0, u64 path_hash, u64 offset, u32 size, u32 key1}.
//   Chunk table: chunk_count x 32 bytes {u64 uncompressed_offset, u32 uncompressed_size, u32 key0, u64 offset,
//     u32 size, u32 key1}.
//   Table rows: XOR with Murmur3_x64_128(seed 42) of {key0, SALT1[1..3]} and of {key1, SALT1[1..3]}.
//   Chunk data: XOR with MD5(Murmur3(the first 16 decrypted chunk-row bytes, key0 restored) ^ SALT2), repeated; then
//     Oodle-compressed (the decompressor is injected: the game's own oo2core_7_win64.dll in the product).
//   Path hash: the first u64 of Murmur3_x64_128(seed 42) of the lower-case path + ".core" (or ".core.stream") + NUL.
// Files are opened with GENERIC_READ and shared read / write only (ArchiveOpenAccess); nothing is ever written.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace st::tex {

void Murmur3x64_128(const void* data, std::size_t size, std::uint32_t seed, std::uint64_t out[2]);
// "ds/foo" -> hash of "ds/foo.core\0"; a path already ending in .core / .stream is hashed as it is (lower case).
std::uint64_t CorePathHash(const std::string& path);
std::string NormalizeCorePath(const std::string& path);
// The masks and the chunk XOR, exposed for the tests' synthetic archive.
void TableMask(std::uint32_t key, std::uint8_t out[16]);
void DecryptTableRow(std::uint8_t* row32, std::uint32_t key0, std::uint32_t key1);   // in place (XOR: also encrypts)
bool ChunkMask(const std::uint8_t key16[16], std::uint8_t out[16]);
// The access mask every archive open uses (tests assert it is read-only).
std::uint32_t ArchiveOpenAccess();
std::uint32_t ArchiveShareMode();

// Decompress `src` into exactly `dst_size` bytes.
using Decompressor = std::function<bool(const std::uint8_t* src, std::size_t src_size, std::uint8_t* dst, std::size_t dst_size)>;

struct ArchiveEntry {
    std::uint64_t hash = 0;
    std::uint64_t offset = 0;   // in the uncompressed stream
    std::uint32_t size = 0;
};

class Archive {
public:
    Archive() = default;
    ~Archive();
    Archive(const Archive&) = delete;
    Archive& operator=(const Archive&) = delete;
    bool Open(const std::wstring& path, std::string& error);
    // Every entry whose hash is in `wanted` (one pass over the entry table).
    void Find(const std::set<std::uint64_t>& wanted, std::vector<ArchiveEntry>& found) const;
    // Bytes [offset, offset+size) of the uncompressed stream.
    bool Read(std::uint64_t offset, std::uint32_t size, const Decompressor& dec, std::vector<std::uint8_t>& out, std::string& error);
    std::uint64_t entry_count() const { return entry_count_; }
    const std::wstring& path() const { return path_; }

private:
    struct Chunk {
        std::uint64_t uoff;
        std::uint32_t usize;
        std::uint64_t coff;
        std::uint32_t csize;
        std::uint8_t key16[16];
    };
    bool LoadChunks(std::string& error);
    bool ReadRaw(std::uint64_t offset, void* dst, std::uint32_t size);
    void* file_ = nullptr;
    std::wstring path_;
    bool encrypted_ = false;
    std::uint64_t entry_count_ = 0;
    std::uint32_t chunk_count_ = 0;
    std::vector<std::uint8_t> etab_, ctab_;
    std::vector<Chunk> chunks_;
};

// All archives of a data folder, opened lazily; a hash is read from the first archive (sorted by file name) that has
// it, like ds_archive.py.
class ArchiveSet {
public:
    bool Open(const std::wstring& data_dir, std::string& error);   // lists *.bin, opens the headers
    // One pass over every archive's entry table for these hashes (call once with everything you will read).
    void Index(const std::set<std::uint64_t>& wanted);
    bool Has(std::uint64_t hash) const { return where_.count(hash) != 0; }
    bool ReadFile(std::uint64_t hash, const Decompressor& dec, std::vector<std::uint8_t>& out, std::string& error);
    std::size_t archive_count() const { return archives_.size(); }

private:
    std::vector<std::unique_ptr<Archive>> archives_;
    std::map<std::uint64_t, std::pair<std::size_t, ArchiveEntry>> where_;
};

}  // namespace st::tex
