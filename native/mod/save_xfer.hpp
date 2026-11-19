// The host-held save session's save transfer, "STX1" (docs/coop-session.md; design M1.5-M1.7).
//
// The host's game plays on the host's own saves; the guest's game never has a save of its own. When the host
// starts a world (Continue, Load, New Game's first save, a pause-menu Load, Retry, a late join) the host mod reads
// that slot's checkpoint.dat READ ONLY and sends it on the link's reliable ordered channel 4 (kChannel); the guest
// mod writes it into its private, redirected session folder and loads it.
//
// Three parts:
//   - the wire (pure): the 8 message kinds with strict decoders (EncodeX / Decode);
//   - the two state machines (pure, no game calls, no file I/O): Sender (host) and Receiver (guest), driven by the
//     game side with decoded messages and polled for what to send (Build / Advance);
//   - helpers: a self-contained SHA-256 (FIPS 180-4, known-answer tested), checkpoint.dat validation, slot names.
// The Windows wrappers (the read-only reader, the guest's sandbox writer, the file-backed part store) are at the end
// of this header and live in save_xfer_io.cpp.
//
// Every message: u8 magic 0x53 | u8 version 1 | u8 kind | u8 flags (0) | u32 xfer_id, little endian, then the body:
//   1 STATE   host->guest  u8 phase, u32 load_counter, u8[32] current_save_sha, u32 offer_xfer_id, char[24] game_build,
//                          u32 savegame_version, u32 save_flags, u8 new_game_pending                      (8 + 74)
//   2 GSTATE  guest->host  u8 phase, u32 loaded_load_counter, u8[32] loaded_sha, u32 have_bytes, u16 refuse_reason
//                                                                                                         (8 + 43)
//   3 OFFER   host->guest  u32 load_counter, u8 slot_type, i8 slot_index, char[24] slot_name, u32 size, u8[32] sha256,
//                          u8 approx, u8 reason, char[24] game_build, u32 savegame_version, u32 save_flags (8 + 100)
//   4 WANT    guest->host  u32 next_offset, u8[32] sha256                                                 (8 + 36)
//   5 CHUNK   host->guest  u32 offset, u16 len (1..1000), data                                       (8 + 6 + len)
//   6 END     host->guest  u32 size, u8[32] sha256                                                        (8 + 36)
//   7 RESULT  guest->host  u8 ok, u16 code                                                                 (8 + 3)
//   8 ABORT   either       u16 code                                                                        (8 + 2)
// (The design's table gave STATE 8+80 and GSTATE 8+44; the fields as listed there add up to 74 and 43, which is what
// is sent.) STATE and GSTATE carry xfer_id 0; every other kind a non-zero xfer_id. The strings are printable ASCII,
// NUL padded (every byte after the first NUL is 0).
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace st::stx {

inline constexpr std::uint8_t kMagic = 0x53;   // 'S'
inline constexpr std::uint8_t kVersion = 1;
inline constexpr unsigned kChannel = 4;        // the link's reliable channel "save handoff" (link_protocol.hpp)
inline constexpr std::size_t kHeaderSize = 8;
inline constexpr std::size_t kMaxChunk = 1000;
inline constexpr std::size_t kMaxMessage = 1024;           // stcoop::link::kMaxReliableMessage
inline constexpr std::uint32_t kMaxSaveBytes = 8u << 20;   // real 100% saves are 1,762,768..1,778,586 bytes
inline constexpr std::size_t kBuildChars = 24;
inline constexpr std::size_t kSlotNameChars = 24;
inline constexpr unsigned kMaxShaRetries = 2;   // after a SHA mismatch at END: WANT 0 at most twice, then RESULT 1
inline constexpr std::uint32_t kCheckpointMagic = 0xC0391E1Du;

inline constexpr std::size_t kStateBody = 74, kGStateBody = 43, kOfferBody = 100, kWantBody = 36, kChunkHead = 6, kEndBody = 36,
                             kResultBody = 3, kAbortBody = 2;

enum class Kind : std::uint8_t { state = 1, gstate = 2, offer = 3, want = 4, chunk = 5, end = 6, result = 7, abort = 8 };
enum class HostPhase : std::uint8_t { title = 0, loading = 1, world = 2, quitting = 3 };
enum class GuestPhase : std::uint8_t { waiting = 0, receiving = 1, loading = 2, world = 3, refused = 4 };
enum class Reason : std::uint8_t { title_continue = 0, title_load = 1, new_game_first_save = 2, in_world_load = 3, retry = 4, late_join = 5 };
enum class ResultCode : std::uint16_t { ok = 0, sha_mismatch = 1, parse = 2, version = 3, disk = 4, load_refused = 5, load_failed = 6 };
enum class AbortCode : std::uint16_t { superseded = 1, cancelled = 2, read_failed = 3, protocol = 4, session_end = 5 };
const char* KindName(Kind k) noexcept;
const char* ResultCodeName(ResultCode c) noexcept;
const char* AbortCodeName(AbortCode c) noexcept;
const char* ReasonName(Reason r) noexcept;

using Sha256 = std::array<std::uint8_t, 32>;

// ---- SHA-256 (self-contained) --------------------------------------------------------------------
class Sha256Ctx {
public:
    Sha256Ctx() noexcept { Reset(); }
    void Reset() noexcept;
    void Update(const void* data, std::size_t size) noexcept;
    Sha256 Final() noexcept;   // the context is reset afterwards

private:
    void Block(const std::uint8_t* p) noexcept;
    std::uint32_t h_[8];
    std::uint8_t buf_[64];
    std::size_t used_ = 0;
    std::uint64_t total_ = 0;
};
Sha256 Sha256Of(const void* data, std::size_t size) noexcept;
std::string ShaHex(const Sha256& sha);   // 64 lower-case hex digits
bool ShaIsZero(const Sha256& sha) noexcept;

// ---- Fields ------------------------------------------------------------------------------------------------
// The game's build string (0x141A13610: "dsq 179/4027081"), SaveGame version (67) and save flags.
struct GameVersion {
    char build[kBuildChars + 1] = {};
    std::uint32_t savegame_version = 0;
    std::uint32_t save_flags = 0;
};
bool SetBuild(GameVersion& v, const char* text) noexcept;   // printable ASCII, 1..24 characters
bool SameVersion(const GameVersion& a, const GameVersion& b) noexcept;

struct Header {
    Kind kind = Kind::state;
    std::uint8_t flags = 0;
    std::uint32_t xfer_id = 0;
};
struct State {
    HostPhase phase = HostPhase::title;
    std::uint32_t load_counter = 0;
    Sha256 current_save_sha{};
    std::uint32_t offer_xfer_id = 0;
    GameVersion version;
    bool new_game_pending = false;
};
struct GState {
    GuestPhase phase = GuestPhase::waiting;
    std::uint32_t loaded_load_counter = 0;
    Sha256 loaded_sha{};
    std::uint32_t have_bytes = 0;
    std::uint16_t refuse_reason = 0;   // a ResultCode (3 = game versions differ)
};
struct Offer {
    std::uint32_t load_counter = 0;
    std::uint8_t slot_type = 0;        // the game's slot type (0xF title Continue, 4 autosave, 1 manual, ...)
    std::int8_t slot_index = 0;        // the game's index (-1 Continue, -2 in-memory checkpoint)
    char slot_name[kSlotNameChars + 1] = {};   // autosaveN / manualsaveN / quicksaveN (IsSlotName)
    std::uint32_t size = 0;            // 1..kMaxSaveBytes
    Sha256 sha{};
    bool approx = false;               // index -2: the newest autosave stands in for the in-memory checkpoint
    Reason reason = Reason::title_continue;
    GameVersion version;
};
struct Want {
    std::uint32_t next_offset = 0;     // 0, or the resume point (bytes the guest already has)
    Sha256 sha{};                      // the offer's, echoed
};
struct Chunk {
    std::uint32_t offset = 0;
    std::uint16_t len = 0;             // 1..kMaxChunk
    const std::uint8_t* data = nullptr;   // points into the decoded buffer
};
struct End {
    std::uint32_t size = 0;
    Sha256 sha{};
};
struct Result {
    bool ok = true;
    ResultCode code = ResultCode::ok;   // ok == (code == ok)
};
struct Abort {
    AbortCode code = AbortCode::cancelled;
};
// One decoded message: `h.kind` says which member is filled.
struct Message {
    Header h;
    State state;
    GState gstate;
    Offer offer;
    Want want;
    Chunk chunk;
    End end;
    Result result;
    Abort abort;
};

// autosaveN / manualsaveN / quicksaveN, N = 0..999 without leading zeros (lower case, as the game names them).
bool IsSlotName(const char* name) noexcept;
bool SetSlotName(Offer& o, const char* name) noexcept;   // false (unchanged) unless IsSlotName

// ---- Encode / decode ---------------------------------------------------------------------------------------
// Each returns the bytes written, 0 when a field is out of range (the decoder would refuse it) or `cap` is short.
std::size_t EncodeState(const State& s, std::uint8_t* out, std::size_t cap) noexcept;
std::size_t EncodeGState(const GState& g, std::uint8_t* out, std::size_t cap) noexcept;
std::size_t EncodeOffer(std::uint32_t xfer_id, const Offer& o, std::uint8_t* out, std::size_t cap) noexcept;
std::size_t EncodeWant(std::uint32_t xfer_id, const Want& w, std::uint8_t* out, std::size_t cap) noexcept;
std::size_t EncodeChunk(std::uint32_t xfer_id, std::uint32_t offset, const std::uint8_t* data, std::size_t len, std::uint8_t* out,
                        std::size_t cap) noexcept;
std::size_t EncodeEnd(std::uint32_t xfer_id, const End& e, std::uint8_t* out, std::size_t cap) noexcept;
std::size_t EncodeResult(std::uint32_t xfer_id, const Result& r, std::uint8_t* out, std::size_t cap) noexcept;
std::size_t EncodeAbort(std::uint32_t xfer_id, AbortCode code, std::uint8_t* out, std::size_t cap) noexcept;
// Strict: magic, version, a known kind, flags 0, the exact length of the kind, xfer_id 0 for STATE / GSTATE and
// non-zero otherwise, every enum in range, every boolean 0 or 1, strings printable and NUL padded, slot names and
// sizes valid, CHUNK within kMaxSaveBytes, RESULT ok == (code == 0). Anything else: false.
bool Decode(const std::uint8_t* data, std::size_t size, Message& out) noexcept;

// ---- checkpoint.dat ---------------------------------------------------------------------------------------
// Three 8-byte NUL-padded decimal sizes (ini, PNG, data) summing with the 24 header bytes to the file size, then the
// [Slot] ini ("Key = Value" lines; ModificationTime required), the PNG, and the data part starting with the u32
// magic 0xC0391E1D.
enum class CheckpointError : std::uint8_t { ok, too_small, too_large, bad_size_field, size_mismatch, bad_magic, bad_ini };
const char* CheckpointErrorName(CheckpointError e) noexcept;
struct CheckpointInfo {
    std::uint32_t ini_size = 0, png_size = 0, data_size = 0;
    std::string title, subtitle;
    std::uint64_t modification_time = 0;
};
CheckpointError ValidateCheckpoint(const std::uint8_t* data, std::size_t size, CheckpointInfo* info = nullptr);

// ---- Sender (host) --------------------------------------------------------------------------------------------
// One transfer at a time. Offer() snapshots the bytes (copied), computes their SHA-256 and queues OFFER; the guest's
// WANT starts (or resumes) the stream of CHUNKs from its next_offset, then END; RESULT ends it. A higher
// load_counter supersedes the current transfer (ABORT superseded for the old xfer_id first). After a link
// reconnect, Reannounce() sends the OFFER again (the guest resumes with WANT). The caller moves messages to the link:
//   while (link window has room && (n = sender.Build(buf, sizeof buf))) { if (!SendOnChannel4(buf, n)) break; sender.Advance(); }
class Sender {
public:
    enum class Phase : std::uint8_t { idle, offered, streaming, ended, done, failed, aborted };
    static const char* PhaseName(Phase p) noexcept;

    // offer.size and offer.sha are filled here from the bytes. False (nothing changes) when the bytes are empty or
    // larger than kMaxSaveBytes, the offer does not encode (slot name, version), or load_counter is lower than the
    // current transfer's (or equal with other bytes). Equal load_counter and the same bytes: Reannounce().
    bool Offer(const st::stx::Offer& offer, const std::uint8_t* data, std::size_t size) noexcept;
    void Reannounce() noexcept;            // the current transfer's OFFER again (new link session)
    void Cancel(AbortCode code) noexcept;  // ABORT for the current transfer (if any is open)
    void OnMessage(const Message& m) noexcept;   // WANT, RESULT, ABORT from the guest; anything else is ignored

    std::size_t Build(std::uint8_t* out, std::size_t cap) const noexcept;   // 0: nothing to send now
    void Advance() noexcept;   // the message Build returned was sent

    Phase phase() const noexcept { return phase_; }
    std::uint32_t xfer_id() const noexcept { return xfer_id_; }
    std::uint32_t load_counter() const noexcept { return offer_.load_counter; }
    const st::stx::Offer& offer() const noexcept { return offer_; }
    std::uint32_t send_offset() const noexcept { return send_offset_; }
    ResultCode result() const noexcept { return result_; }
    AbortCode abort_code() const noexcept { return abort_code_; }
    bool busy() const noexcept { return phase_ == Phase::offered || phase_ == Phase::streaming || phase_ == Phase::ended; }
    bool has_output() const noexcept;   // Build would return a message
    // Counters for the log and the metrics.
    std::uint64_t chunks_sent = 0, bytes_sent = 0, resumes = 0, restarts = 0, stale = 0;
    void set_next_xfer_id(std::uint32_t id) noexcept { next_id_ = id ? id : 1; }   // tests

private:
    std::uint32_t NewId() noexcept;
    st::stx::Offer offer_;
    std::vector<std::uint8_t> data_;
    std::deque<std::vector<std::uint8_t>> control_;   // ABORT / OFFER, sent before any CHUNK
    Phase phase_ = Phase::idle;
    std::uint32_t xfer_id_ = 0, next_id_ = 1, send_offset_ = 0;
    bool end_pending_ = false;
    ResultCode result_ = ResultCode::ok;
    AbortCode abort_code_ = AbortCode::cancelled;
};

// ---- Receiver (guest) -----------------------------------------------------------------------------------------
// Where the received bytes go while a transfer runs (the guest's incoming\<sha-hex>.part). Open keeps bytes already
// there for the same SHA (resume) and reports how many; another SHA's bytes are dropped.
class PartStore {
public:
    virtual ~PartStore() = default;
    virtual bool Open(const Sha256& sha, std::uint32_t size, std::uint32_t& have) noexcept = 0;
    virtual bool Append(std::uint32_t offset, const std::uint8_t* data, std::size_t len) noexcept = 0;
    virtual bool ReadAll(std::vector<std::uint8_t>& out) noexcept = 0;
    virtual void Remove() noexcept = 0;
};
// In memory (tests; or a guest that keeps the transfer in RAM).
class MemoryPartStore final : public PartStore {
public:
    bool Open(const Sha256& sha, std::uint32_t size, std::uint32_t& have) noexcept override;
    bool Append(std::uint32_t offset, const std::uint8_t* data, std::size_t len) noexcept override;
    bool ReadAll(std::vector<std::uint8_t>& out) noexcept override;
    void Remove() noexcept override;
    std::vector<std::uint8_t> bytes;   // tests may corrupt them
    Sha256 sha{};
    bool open = false;
    bool fail_append = false;   // tests: a disk error
    unsigned removes = 0;
};

// Takes the host's messages in order (OFFER, CHUNK, END, ABORT, STATE); answers with WANT / RESULT / ABORT
// (Build / Advance, as the Sender). In order:
//  - an OFFER from another game build / save version: RESULT version, nothing stored (refused);
//  - a new OFFER: WANT 0, or WANT <bytes already there> (resume); an OFFER of the transfer already running (a new
//    link session): WANT <bytes received>; a higher load_counter supersedes the running transfer (its part is
//    removed); a lower one is ignored; the save already loaded (same load_counter and SHA): RESULT ok again;
//  - CHUNKs are taken only in order (offset == bytes received); others are dropped (a WANT restarted the stream);
//  - END: the SHA-256 of the whole part; a mismatch removes the part and asks again (WANT 0) at most kMaxShaRetries
//    times, then RESULT sha_mismatch and the guest stays waiting; then ValidateCheckpoint (RESULT parse);
//  - complete: TakeComplete() hands the bytes to the game side (which writes the slot with WriteGuestSaveFile and
//    loads it) and LoadDone(code) sends the RESULT.
class Receiver {
public:
    enum class Phase : std::uint8_t { waiting, receiving, complete, loading, loaded, refused, failed };
    static const char* PhaseName(Phase p) noexcept;

    explicit Receiver(PartStore& store) noexcept : store_(store) {}
    void SetLocalVersion(const GameVersion& v) noexcept {
        local_ = v;
        have_local_ = true;
    }
    void OnMessage(const Message& m) noexcept;
    // phase complete -> loading. False while the previous TakeComplete's load has no LoadDone yet: one load at a time,
    // so each RESULT names the transfer the game really loaded (a newer save waits, complete, until then).
    bool TakeComplete(std::vector<std::uint8_t>& data, st::stx::Offer& offer) noexcept;
    void LoadDone(ResultCode code) noexcept;   // the load TakeComplete started: loaded (ok) or failed; sends its RESULT
    void Reset() noexcept;                      // everything dropped (tests)
    // A new link session (a new link generation: the host may be a restarted process whose load counters and xfer ids
    // start over): the running transfer is forgotten (its part's bytes stay for a resume of the same file), so the next
    // OFFER is taken whatever its load_counter; a running load and the save already loaded are kept.
    void NewSession() noexcept;
    bool loading() const noexcept { return loading_xfer_ != 0; }

    std::size_t Build(std::uint8_t* out, std::size_t cap) const noexcept;
    void Advance() noexcept;
    bool has_output() const noexcept { return !out_.empty(); }

    Phase phase() const noexcept { return phase_; }
    GState Status() const noexcept;   // for the 1 Hz GSTATE (the game side may say world itself)
    std::uint32_t have_bytes() const noexcept { return have_; }
    const st::stx::Offer& offer() const noexcept { return offer_; }
    std::uint32_t xfer_id() const noexcept { return xfer_; }
    bool have_host_state() const noexcept { return have_state_; }
    const State& host_state() const noexcept { return state_; }
    bool version_mismatch() const noexcept { return version_mismatch_; }
    ResultCode last_error() const noexcept { return error_; }
    std::uint64_t dropped_chunks = 0, stale = 0, sha_retries = 0, resumes = 0;

private:
    void Queue(std::size_t n, const std::uint8_t* buf) noexcept;
    void SendWant() noexcept;
    void SendResult(std::uint32_t xfer, ResultCode code) noexcept;
    void Fail(ResultCode code) noexcept;
    void Drop() noexcept;   // the running transfer's part removed, back to waiting

    PartStore& store_;
    GameVersion local_;
    bool have_local_ = false;
    State state_;
    bool have_state_ = false;
    bool version_mismatch_ = false;
    st::stx::Offer offer_, loading_offer_;
    std::uint32_t xfer_ = 0, have_ = 0, loading_xfer_ = 0;
    unsigned retries_ = 0;
    Phase phase_ = Phase::waiting;
    ResultCode error_ = ResultCode::ok;
    std::vector<std::uint8_t> complete_;
    bool loaded_ = false;
    std::uint32_t loaded_counter_ = 0;
    Sha256 loaded_sha_{};
    std::deque<std::vector<std::uint8_t>> out_;
};

// ---- Paths below the guest's redirected save folder (pure) -------------------------------------------------
// The only files the guest's mod ever writes:
//   incoming\<64 lower-case hex>.part                          the transfer in progress
//   DeathStrandingDC\<account digits>\manualsave<N>\checkpoint.dat   the slot it loads (never an autosave: the guest
//                                                                    game's own autosaves rotate through those)
enum class GuestPath : std::uint8_t { invalid, incoming_part, manual_slot };
GuestPath ClassifyGuestPath(const std::wstring& rel) noexcept;
std::wstring IncomingPartRel(const Sha256& sha);
std::wstring ManualSlotRel(std::uint32_t account, int slot);   // "" for account 0 or slot outside 0..999

// ---- Windows wrappers (save_xfer_io.cpp) ----------------------------------------------------------------------
// The host's real save files are opened ONLY by ReadSaveFileReadOnly: CreateFileW with GENERIC_READ and
// FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE. There is no write path for them. OpenAccessAllowed is the
// assertion every open goes through (unit-tested: nothing else passes).
// Full sharing does NOT keep the game's save from failing while a read handle is open: the game writes
// <slot>\checkpoint.dattemp and replaces checkpoint.dat with ONE MoveFileExW(dattemp, checkpoint.dat, REPLACE_EXISTING |
// COPY_ALLOWED | WRITE_THROUGH) (0x141A01ED0 -> 0x14192AEA0 -> the file device's slot 0x140 = 0x14192B170; no retry; on
// failure the .dattemp is deleted and that save is lost), and a rename over a file that has an open handle fails with
// ERROR_ACCESS_DENIED whatever its share flags (tests/save_xfer_tests.cpp). So the reader keeps that window as short as
// it can: it refuses (busy) while a checkpoint.dattemp sits next to the file (the game is writing that slot), opens,
// reads into memory and closes at once, and never holds a handle across a wait.
inline constexpr unsigned long kSaveReadAccess = 0x80000000ul;   // GENERIC_READ
inline constexpr unsigned long kSaveReadShare = 0x7ul;           // FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE
bool OpenAccessAllowed(unsigned long access, unsigned long share) noexcept;
enum class ReadStatus : std::uint8_t { ok, open_failed, too_large, read_failed, unstable, busy };
const char* ReadStatusName(ReadStatus s) noexcept;
// busy: <path>temp (checkpoint.dattemp) exists, nothing was opened.
ReadStatus ReadSaveFileReadOnly(const std::wstring& path, std::vector<std::uint8_t>& out, std::uint32_t max_bytes = kMaxSaveBytes);
// Read, wait settle_ms, read again: equal SHA-256 = a whole file (the game writes checkpoint.dattemp and renames it);
// otherwise (or busy) up to `tries` times. Blocks: never on the game thread.
ReadStatus ReadSaveSnapshot(const std::wstring& path, std::vector<std::uint8_t>& out, unsigned settle_ms = 400, unsigned tries = 3);

// Where the guest may write: the redirect's own folder, while the redirect is confirmed. The game side fills it from
// st::automation::SavesScratchRoot() / SavesRedirected() and the real roots (%USERPROFILE%\AppData\Local\
// KojimaProductions, %LOCALAPPDATA%\KojimaProductions).
struct SandboxTarget {
    std::wstring scratch_root;
    bool redirected = false;
    std::vector<std::wstring> real_roots;
};
// "" when writing below target.scratch_root is allowed; otherwise why not (not redirected, no root, the root overlaps
// a real root, not absolute).
std::string SandboxTargetProblem(const SandboxTarget& target);
// Writes `rel` (ClassifyGuestPath: incoming part or manual slot only) below the scratch root: a temp file next to it,
// flushed, MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH), read back and compared by SHA-256. Refuses (false + why)
// unless SandboxTargetProblem is "" and the path is allowed. Never the real-root spelling.
bool WriteGuestSaveFile(const SandboxTarget& target, const std::wstring& rel, const std::uint8_t* data, std::size_t size, std::string* why);
// The guest's part store on disk: <scratch>\incoming\<sha-hex>.part, every write through the same checks.
class SandboxPartStore final : public PartStore {
public:
    explicit SandboxPartStore(SandboxTarget target) : target_(std::move(target)) {}
    bool Open(const Sha256& sha, std::uint32_t size, std::uint32_t& have) noexcept override;
    bool Append(std::uint32_t offset, const std::uint8_t* data, std::size_t len) noexcept override;
    bool ReadAll(std::vector<std::uint8_t>& out) noexcept override;
    void Remove() noexcept override;
    const std::string& last_error() const noexcept { return error_; }

private:
    SandboxTarget target_;
    std::wstring rel_;
    std::uint32_t size_ = 0;
    std::string error_;
};

}  // namespace st::stx
