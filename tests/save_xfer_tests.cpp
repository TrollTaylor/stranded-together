// STX1 (native/mod/save_xfer.*), the host-held save's transfer: SHA-256 known answers, every decoder rejection,
// the sender and receiver state machines against each other (resume, supersede, SHA mismatch retries, version
// refusal, disk errors), checkpoint.dat validation, the guest paths, the link's burst cap, and the Windows wrappers
// (read-only reader, sandbox writer, part store) on temporary folders. Never a real save.
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "coop_rules.hpp"
#include "save_xfer.hpp"
#include "stcoop/link_protocol.hpp"

using namespace st::stx;

namespace {
int g_checks = 0, g_fail = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        ++g_checks;                                                      \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            ++g_fail;                                                    \
        }                                                                \
    } while (0)

using Bytes = std::vector<std::uint8_t>;

// A checkpoint.dat as the game writes it: 3 x 8-byte decimal sizes, [Slot] ini, PNG, data (magic + payload).
Bytes MakeCheckpoint(std::size_t payload = 3000, std::uint8_t seed = 7, const char* time = "63865424165000000") {
    std::string ini = "[Slot]\r\nTitle                           = Order 79\r\nSubTitle                        = Auto Save\r\n"
                      "ModificationTime                = ";
    ini += time;
    ini += "\r\n";
    Bytes png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 1, 2, 3, 4};
    Bytes data = {0x1D, 0x1E, 0x39, 0xC0};
    for (std::size_t i = 0; i < payload; ++i) data.push_back(static_cast<std::uint8_t>((i * 31 + seed) & 0xFF));
    Bytes out(24, 0);
    const auto put = [&](std::size_t at, std::size_t v) {
        const std::string s = std::to_string(v);
        std::memcpy(out.data() + at, s.data(), s.size());
    };
    put(0, ini.size());
    put(8, png.size());
    put(16, data.size());
    out.insert(out.end(), ini.begin(), ini.end());
    out.insert(out.end(), png.begin(), png.end());
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

GameVersion Version(const char* build = "dsq 179/4027081", std::uint32_t ver = 67, std::uint32_t flags = 0) {
    GameVersion v;
    SetBuild(v, build);
    v.savegame_version = ver;
    v.save_flags = flags;
    return v;
}

Offer MakeOffer(std::uint32_t load_counter, const char* slot = "manualsave0", Reason reason = Reason::title_continue) {
    Offer o;
    o.load_counter = load_counter;
    o.slot_type = 0xF;
    o.slot_index = -1;
    SetSlotName(o, slot);
    o.reason = reason;
    o.version = Version();
    return o;
}

// A one-way pipe of encoded messages, decoded on arrival (as the link's channel 4 would hand them over).
template <class From>
std::vector<Bytes> Drain(From& from, unsigned limit = 100000) {
    std::vector<Bytes> out;
    std::uint8_t buf[kMaxMessage];
    while (limit--) {
        const std::size_t n = from.Build(buf, sizeof(buf));
        if (!n) break;
        out.emplace_back(buf, buf + n);
        from.Advance();
    }
    return out;
}

template <class To>
void Deliver(To& to, const std::vector<Bytes>& msgs) {
    for (const Bytes& b : msgs) {
        Message m;
        if (Decode(b.data(), b.size(), m)) to.OnMessage(m);
    }
}

Kind KindOf(const Bytes& b) {
    Message m;
    return Decode(b.data(), b.size(), m) ? m.h.kind : static_cast<Kind>(0);
}

// ---- SHA-256 ----------------------------------------------------------------------------------------------------
void TestSha256() {
    CHECK(ShaHex(Sha256Of("", 0)) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(ShaHex(Sha256Of("abc", 3)) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const char* two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(ShaHex(Sha256Of(two, std::strlen(two))) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    const char* four = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
    CHECK(ShaHex(Sha256Of(four, std::strlen(four))) == "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
    // One million 'a', fed in odd pieces (the block boundary paths).
    Sha256Ctx c;
    const std::string piece(997, 'a');
    std::size_t left = 1000000;
    while (left) {
        const std::size_t n = left < piece.size() ? left : piece.size();
        c.Update(piece.data(), n);
        left -= n;
    }
    CHECK(ShaHex(c.Final()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    // 55 / 56 / 64 bytes: the padding edges.
    const std::string a55(55, 'a'), a56(56, 'a'), a64(64, 'a');
    CHECK(ShaHex(Sha256Of(a55.data(), 55)) == "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    CHECK(ShaHex(Sha256Of(a56.data(), 56)) == "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
    CHECK(ShaHex(Sha256Of(a64.data(), 64)) == "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
    CHECK(ShaIsZero(Sha256{}) && !ShaIsZero(Sha256Of("", 0)));
}

// ---- Wire ----------------------------------------------------------------------------------------------------------
void TestRoundTrips() {
    std::uint8_t buf[kMaxMessage];
    Message m;
    State s;
    s.phase = HostPhase::loading;
    s.load_counter = 7;
    s.current_save_sha = Sha256Of("x", 1);
    s.offer_xfer_id = 3;
    s.version = Version();
    s.new_game_pending = true;
    std::size_t n = EncodeState(s, buf, sizeof(buf));
    CHECK(n == kHeaderSize + kStateBody && n == 82);
    CHECK(Decode(buf, n, m) && m.h.kind == Kind::state && m.h.xfer_id == 0 && m.state.phase == HostPhase::loading && m.state.load_counter == 7 &&
          m.state.current_save_sha == s.current_save_sha && m.state.offer_xfer_id == 3 && SameVersion(m.state.version, s.version) &&
          m.state.new_game_pending);

    GState g;
    g.phase = GuestPhase::receiving;
    g.loaded_load_counter = 2;
    g.loaded_sha = Sha256Of("y", 1);
    g.have_bytes = 5000;
    g.refuse_reason = 3;
    n = EncodeGState(g, buf, sizeof(buf));
    CHECK(n == kHeaderSize + kGStateBody && n == 51);
    CHECK(Decode(buf, n, m) && m.h.kind == Kind::gstate && m.gstate.phase == GuestPhase::receiving && m.gstate.have_bytes == 5000 &&
          m.gstate.refuse_reason == 3 && m.gstate.loaded_sha == g.loaded_sha);

    Offer o = MakeOffer(9, "autosave18", Reason::in_world_load);
    o.slot_index = -2;
    o.approx = true;
    o.size = 1762768;
    o.sha = Sha256Of("z", 1);
    n = EncodeOffer(42, o, buf, sizeof(buf));
    CHECK(n == kHeaderSize + kOfferBody && n == 108);
    CHECK(Decode(buf, n, m) && m.h.kind == Kind::offer && m.h.xfer_id == 42 && m.offer.load_counter == 9 && m.offer.slot_index == -2 &&
          std::strcmp(m.offer.slot_name, "autosave18") == 0 && m.offer.size == 1762768 && m.offer.approx &&
          m.offer.reason == Reason::in_world_load && SameVersion(m.offer.version, o.version) && m.offer.sha == o.sha);

    Want w;
    w.next_offset = 4000;
    w.sha = o.sha;
    n = EncodeWant(42, w, buf, sizeof(buf));
    CHECK(n == 44 && Decode(buf, n, m) && m.h.kind == Kind::want && m.want.next_offset == 4000 && m.want.sha == o.sha);

    std::uint8_t data[kMaxChunk];
    for (std::size_t i = 0; i < sizeof(data); ++i) data[i] = static_cast<std::uint8_t>(i);
    n = EncodeChunk(42, 1000, data, kMaxChunk, buf, sizeof(buf));
    CHECK(n == kHeaderSize + kChunkHead + kMaxChunk && n <= kMaxMessage && n <= stcoop::link::kMaxReliableMessage);
    CHECK(Decode(buf, n, m) && m.h.kind == Kind::chunk && m.chunk.offset == 1000 && m.chunk.len == kMaxChunk &&
          std::memcmp(m.chunk.data, data, kMaxChunk) == 0);

    End e;
    e.size = 1762768;
    e.sha = o.sha;
    n = EncodeEnd(42, e, buf, sizeof(buf));
    CHECK(n == 44 && Decode(buf, n, m) && m.h.kind == Kind::end && m.end.size == 1762768 && m.end.sha == o.sha);

    Result r;
    r.ok = false;
    r.code = ResultCode::load_failed;
    n = EncodeResult(42, r, buf, sizeof(buf));
    CHECK(n == 11 && Decode(buf, n, m) && m.h.kind == Kind::result && !m.result.ok && m.result.code == ResultCode::load_failed);
    n = EncodeAbort(42, AbortCode::superseded, buf, sizeof(buf));
    CHECK(n == 10 && Decode(buf, n, m) && m.h.kind == Kind::abort && m.abort.code == AbortCode::superseded);

    // Encoders refuse what the decoder would refuse.
    CHECK(EncodeOffer(0, o, buf, sizeof(buf)) == 0);   // xfer_id 0
    Offer bad = o;
    bad.size = 0;
    CHECK(EncodeOffer(1, bad, buf, sizeof(buf)) == 0);
    bad = o;
    bad.size = kMaxSaveBytes + 1;
    CHECK(EncodeOffer(1, bad, buf, sizeof(buf)) == 0);
    bad = o;
    bad.slot_name[0] = 'x';
    CHECK(EncodeOffer(1, bad, buf, sizeof(buf)) == 0);
    bad = o;
    bad.version.build[0] = 0;
    CHECK(EncodeOffer(1, bad, buf, sizeof(buf)) == 0);
    CHECK(EncodeChunk(1, 0, data, 0, buf, sizeof(buf)) == 0 && EncodeChunk(1, 0, data, kMaxChunk + 1, buf, sizeof(buf)) == 0);
    CHECK(EncodeChunk(1, kMaxSaveBytes - 10, data, 11, buf, sizeof(buf)) == 0);
    CHECK(EncodeChunk(1, 0, data, 100, buf, 50) == 0);   // short buffer
    Result incoherent;
    incoherent.ok = true;
    incoherent.code = ResultCode::disk;
    CHECK(EncodeResult(1, incoherent, buf, sizeof(buf)) == 0);
    CHECK(EncodeAbort(1, static_cast<AbortCode>(0), buf, sizeof(buf)) == 0 && EncodeAbort(1, static_cast<AbortCode>(6), buf, sizeof(buf)) == 0);
    CHECK(EncodeAbort(0, AbortCode::cancelled, buf, sizeof(buf)) == 0);
    GState gbad;
    gbad.phase = static_cast<GuestPhase>(5);
    CHECK(EncodeGState(gbad, buf, sizeof(buf)) == 0);
    GameVersion v;
    CHECK(!SetBuild(v, "") && !SetBuild(v, "0123456789012345678901234") && !SetBuild(v, "tab\there") && SetBuild(v, "012345678901234567890123"));
}

// Every rejection of the strict decoder.
void TestDecoderRejections() {
    std::uint8_t buf[kMaxMessage];
    Message m;
    Offer o = MakeOffer(1);
    o.size = 100;
    const std::size_t n = EncodeOffer(5, o, buf, sizeof(buf));
    CHECK(Decode(buf, n, m));
    const auto mutated = [&](std::size_t at, std::uint8_t value) {
        Bytes b(buf, buf + n);
        b[at] = value;
        Message x;
        return Decode(b.data(), b.size(), x);
    };
    CHECK(!Decode(nullptr, n, m) && !Decode(buf, 0, m) && !Decode(buf, kHeaderSize - 1, m));
    CHECK(!mutated(0, 0x54));    // magic
    CHECK(!mutated(1, 2));       // version
    CHECK(!mutated(2, 0));       // kind 0
    CHECK(!mutated(2, 9));       // kind 9
    CHECK(!mutated(3, 1));       // flags
    {
        Bytes b(buf, buf + n);
        std::memset(b.data() + 4, 0, 4);   // xfer_id 0 on an OFFER
        CHECK(!Decode(b.data(), b.size(), m));
    }
    CHECK(!Decode(buf, n - 1, m));   // short by one
    {
        Bytes b(buf, buf + n);
        b.push_back(0);   // one byte too many
        CHECK(!Decode(b.data(), b.size(), m));
    }
    // OFFER body: slot name, size, approx, reason, build.
    const std::size_t body = kHeaderSize;
    CHECK(!mutated(body + 6, 'x'));                 // slot name "xanualsave0"
    CHECK(!mutated(body + 6 + 12, 'z'));            // text after the NUL
    CHECK(!mutated(body + 6, 0));                   // empty slot name
    CHECK(!mutated(body + 7, 0x01));                // control character
    {
        Bytes b(buf, buf + n);
        std::memset(b.data() + body + 30, 0, 4);    // size 0
        CHECK(!Decode(b.data(), b.size(), m));
        const std::uint32_t big = kMaxSaveBytes + 1;
        std::memcpy(b.data() + body + 30, &big, 4);
        CHECK(!Decode(b.data(), b.size(), m));
    }
    CHECK(!mutated(body + 66, 2));                  // approx 2
    CHECK(!mutated(body + 67, 6));                  // reason 6
    CHECK(!mutated(body + 68, 0));                  // empty build
    CHECK(!mutated(body + 68, 0x80));               // non-ASCII build
    // STATE / GSTATE: xfer_id must be 0; phases and booleans in range.
    State s;
    s.version = Version();
    std::uint8_t sb[kMaxMessage];
    const std::size_t sn = EncodeState(s, sb, sizeof(sb));
    CHECK(Decode(sb, sn, m));
    {
        Bytes b(sb, sb + sn);
        b[4] = 1;
        CHECK(!Decode(b.data(), b.size(), m));
        b[4] = 0;
        b[8] = 4;   // phase 4
        CHECK(!Decode(b.data(), b.size(), m));
        b[8] = 0;
        b[sn - 1] = 2;   // new_game_pending 2
        CHECK(!Decode(b.data(), b.size(), m));
        b[sn - 1] = 0;
        b.pop_back();
        CHECK(!Decode(b.data(), b.size(), m));
    }
    GState g;
    const std::size_t gn = EncodeGState(g, sb, sizeof(sb));
    CHECK(Decode(sb, gn, m));
    {
        Bytes b(sb, sb + gn);
        b[8] = 5;
        CHECK(!Decode(b.data(), b.size(), m));
        b[8] = 0;
        b[gn - 2] = 7;   // refuse_reason 7
        CHECK(!Decode(b.data(), b.size(), m));
        b[gn - 2] = 0;
        const std::uint32_t big = kMaxSaveBytes + 1;
        std::memcpy(b.data() + 8 + 37, &big, 4);   // have_bytes
        CHECK(!Decode(b.data(), b.size(), m));
    }
    // WANT: offset beyond the largest save; wrong length.
    Want w;
    std::size_t wn = EncodeWant(3, w, sb, sizeof(sb));
    CHECK(Decode(sb, wn, m));
    {
        Bytes b(sb, sb + wn);
        const std::uint32_t big = kMaxSaveBytes + 1;
        std::memcpy(b.data() + 8, &big, 4);
        CHECK(!Decode(b.data(), b.size(), m));
        CHECK(!Decode(sb, wn - 1, m));
    }
    // CHUNK: len 0, len > 1000, length disagreeing with len, past the largest save.
    std::uint8_t data[kMaxChunk] = {};
    std::size_t cn = EncodeChunk(3, 0, data, 10, sb, sizeof(sb));
    CHECK(Decode(sb, cn, m));
    {
        Bytes b(sb, sb + cn);
        b[12] = 0;
        b[13] = 0;   // len 0
        CHECK(!Decode(b.data(), b.size(), m));
        b[12] = 11;   // len 11 with 10 bytes
        CHECK(!Decode(b.data(), b.size(), m));
        b[12] = 9;
        CHECK(!Decode(b.data(), b.size(), m));
        Bytes big(kHeaderSize + kChunkHead + kMaxChunk + 1, 0);
        std::memcpy(big.data(), sb, 8);
        const std::uint16_t len = kMaxChunk + 1;
        std::memcpy(big.data() + 12, &len, 2);
        CHECK(!Decode(big.data(), big.size(), m));
        b[12] = 10;
        const std::uint32_t off = kMaxSaveBytes - 5;
        std::memcpy(b.data() + 8, &off, 4);
        CHECK(!Decode(b.data(), b.size(), m));
        CHECK(!Decode(sb, kHeaderSize + kChunkHead, m));   // no data at all
    }
    // END: size 0 / too large.
    End e;
    e.size = 10;
    std::size_t en = EncodeEnd(3, e, sb, sizeof(sb));
    CHECK(Decode(sb, en, m));
    {
        Bytes b(sb, sb + en);
        std::memset(b.data() + 8, 0, 4);
        CHECK(!Decode(b.data(), b.size(), m));
        const std::uint32_t big = kMaxSaveBytes + 1;
        std::memcpy(b.data() + 8, &big, 4);
        CHECK(!Decode(b.data(), b.size(), m));
    }
    // RESULT: ok and code must agree; code in range; ok 0/1.
    Result r;
    std::size_t rn = EncodeResult(3, r, sb, sizeof(sb));
    CHECK(Decode(sb, rn, m) && m.result.ok);
    {
        Bytes b(sb, sb + rn);
        b[8] = 0;   // not ok with code 0
        CHECK(!Decode(b.data(), b.size(), m));
        b[8] = 1;
        b[9] = 4;   // ok with code 4
        CHECK(!Decode(b.data(), b.size(), m));
        b[8] = 0;
        b[9] = 7;   // code 7
        CHECK(!Decode(b.data(), b.size(), m));
        b[8] = 2;
        b[9] = 1;
        CHECK(!Decode(b.data(), b.size(), m));
        b[8] = 0;
        b[9] = 1;
        CHECK(Decode(b.data(), b.size(), m) && m.result.code == ResultCode::sha_mismatch);
    }
    // ABORT: code 0 / 6; xfer_id 0.
    std::size_t an = EncodeAbort(3, AbortCode::cancelled, sb, sizeof(sb));
    CHECK(Decode(sb, an, m));
    {
        Bytes b(sb, sb + an);
        b[8] = 0;
        CHECK(!Decode(b.data(), b.size(), m));
        b[8] = 6;
        CHECK(!Decode(b.data(), b.size(), m));
        b[8] = 1;
        std::memset(b.data() + 4, 0, 4);
        CHECK(!Decode(b.data(), b.size(), m));
    }
    // Longer than a reliable message: never.
    Bytes huge(kMaxMessage + 1, 0);
    std::memcpy(huge.data(), buf, 8);
    CHECK(!Decode(huge.data(), huge.size(), m));
}

void TestSlotNamesAndPaths() {
    CHECK(IsSlotName("autosave0") && IsSlotName("autosave22") && IsSlotName("manualsave20") && IsSlotName("quicksave2") && IsSlotName("manualsave999"));
    CHECK(!IsSlotName("autosave") && !IsSlotName("autosave01") && !IsSlotName("autosave1000") && !IsSlotName("AutoSave1") &&
          !IsSlotName("profile") && !IsSlotName("autosave1x") && !IsSlotName(nullptr) && !IsSlotName("manualsave-1"));
    const Sha256 sha = Sha256Of("abc", 3);
    CHECK(ClassifyGuestPath(IncomingPartRel(sha)) == GuestPath::incoming_part);
    CHECK(ClassifyGuestPath(ManualSlotRel(104729, 5)) == GuestPath::manual_slot);
    CHECK(ManualSlotRel(104729, 6) == L"DeathStrandingDC\\104729\\manualsave6\\checkpoint.dat");
    CHECK(ManualSlotRel(0, 5).empty() && ManualSlotRel(1, -1).empty() && ManualSlotRel(1, 1000).empty());
    for (const wchar_t* bad : {L"", L"DeathStrandingDC\\104729\\autosave0\\checkpoint.dat", L"DeathStrandingDC\\104729\\quicksave0\\checkpoint.dat",
                               L"DeathStrandingDC\\104729\\manualsave5\\profile.dat", L"DeathStrandingDC\\104729\\profile\\profile.dat",
                               L"DeathStrandingDC\\..\\manualsave5\\checkpoint.dat", L"..\\incoming\\x.part", L"incoming\\abc.part",
                               L"C:\\Users\\player\\AppData\\Local\\KojimaProductions\\DeathStrandingDC\\104729\\manualsave5\\checkpoint.dat",
                               L"\\DeathStrandingDC\\104729\\manualsave5\\checkpoint.dat", L"DeathStrandingDC/104729/manualsave5/checkpoint.dat",
                               L"DeathStrandingDC\\0\\manualsave5\\checkpoint.dat", L"DeathStrandingDC\\104729\\manualsave05\\checkpoint.dat",
                               L"DeathStrandingDC\\x\\manualsave5\\checkpoint.dat", L"incoming\\sub\\a.part"})
        CHECK(ClassifyGuestPath(bad) == GuestPath::invalid);
    std::wstring upper = IncomingPartRel(sha);
    for (wchar_t& c : upper)
        if (c >= L'a' && c <= L'f') c = static_cast<wchar_t>(c - 32);
    CHECK(ClassifyGuestPath(upper) == GuestPath::invalid);   // lower-case hex only
}

void TestCheckpoint() {
    const Bytes good = MakeCheckpoint();
    CheckpointInfo info;
    CHECK(ValidateCheckpoint(good.data(), good.size(), &info) == CheckpointError::ok);
    CHECK(info.modification_time == 63865424165000000ull && info.title == "Order 79" && info.subtitle == "Auto Save" && info.png_size == 12);
    CHECK(ValidateCheckpoint(good.data(), 10) == CheckpointError::too_small);
    CHECK(ValidateCheckpoint(nullptr, 0) == CheckpointError::too_small);
    Bytes b = good;
    b.push_back(0);   // one byte more than the sizes say
    CHECK(ValidateCheckpoint(b.data(), b.size()) == CheckpointError::size_mismatch);
    b = good;
    b.pop_back();
    CHECK(ValidateCheckpoint(b.data(), b.size()) == CheckpointError::size_mismatch);
    b = good;
    b[0] = 'x';   // not a number
    CHECK(ValidateCheckpoint(b.data(), b.size()) == CheckpointError::bad_size_field);
    b = good;
    b[7] = '1';   // bytes after the NUL are the game's leftovers, not checked (run 20261119-074751: "410\0p\x05\0\0")
    CHECK(ValidateCheckpoint(b.data(), b.size()) == CheckpointError::ok);
    b = good;
    b[4] = 'p';
    b[5] = 0x05;   // the real autosave's leftovers after the NUL
    CHECK(ValidateCheckpoint(b.data(), b.size()) == CheckpointError::ok);
    b = good;
    b[3] = 'x';   // no NUL right after the digits
    CHECK(ValidateCheckpoint(b.data(), b.size()) == CheckpointError::bad_size_field);
    b = good;
    b[8] = 0;   // empty field
    CHECK(ValidateCheckpoint(b.data(), b.size()) == CheckpointError::bad_size_field);
    b = good;
    std::memcpy(b.data() + 16, "0", 1);   // a leading zero ("0" + the old digits)
    CHECK(ValidateCheckpoint(b.data(), b.size()) == CheckpointError::bad_size_field);
    {
        // The data magic.
        b = good;
        std::uint32_t ini = 0, png = 0;
        ini = static_cast<std::uint32_t>(std::atoi(reinterpret_cast<const char*>(good.data())));
        png = static_cast<std::uint32_t>(std::atoi(reinterpret_cast<const char*>(good.data() + 8)));
        b[24 + ini + png] ^= 0xFF;
        CHECK(ValidateCheckpoint(b.data(), b.size()) == CheckpointError::bad_magic);
        // The ini: header, ModificationTime, characters.
        b = good;
        b[25] = 'X';   // "[Xlot]"
        CHECK(ValidateCheckpoint(b.data(), b.size()) == CheckpointError::bad_ini);
        b = good;
        b[30] = 0x01;
        CHECK(ValidateCheckpoint(b.data(), b.size()) == CheckpointError::bad_ini);
    }
    const Bytes no_time = MakeCheckpoint(100, 1, "");
    CHECK(ValidateCheckpoint(no_time.data(), no_time.size()) == CheckpointError::bad_ini);
    const Bytes bad_time = MakeCheckpoint(100, 1, "12a4");
    CHECK(ValidateCheckpoint(bad_time.data(), bad_time.size()) == CheckpointError::bad_ini);
}

// ---- State machines ----------------------------------------------------------------------------------------------
struct Pair {
    Sender host;
    MemoryPartStore store;
    Receiver guest{store};
    Pair() { guest.SetLocalVersion(Version()); }
    // Moves everything both ways until nothing is left (the reliable channel delivers in order).
    void Run(unsigned rounds = 100) {
        for (unsigned i = 0; i < rounds; ++i) {
            const auto down = Drain(host);
            Deliver(guest, down);
            const auto up = Drain(guest);
            Deliver(host, up);
            if (down.empty() && up.empty()) return;
        }
    }
};

void TestTransfer() {
    Pair p;
    const Bytes save = MakeCheckpoint(4321);   // several chunks, the last one short
    CHECK(p.host.Offer(MakeOffer(1), save.data(), save.size()));
    CHECK(p.host.phase() == Sender::Phase::offered && p.host.offer().size == save.size() && p.host.offer().sha == Sha256Of(save.data(), save.size()));
    p.Run();
    CHECK(p.guest.phase() == Receiver::Phase::complete && p.store.bytes == save);
    CHECK(p.host.phase() == Sender::Phase::ended && p.host.chunks_sent == (save.size() + kMaxChunk - 1) / kMaxChunk);
    CHECK(p.guest.Status().phase == GuestPhase::loading);
    Bytes got;
    Offer offer;
    CHECK(p.guest.TakeComplete(got, offer) && got == save && offer.load_counter == 1 && p.guest.phase() == Receiver::Phase::loading);
    CHECK(!p.guest.TakeComplete(got, offer));
    p.guest.LoadDone(ResultCode::ok);
    p.Run();
    CHECK(p.host.phase() == Sender::Phase::done && p.host.result() == ResultCode::ok);
    CHECK(p.guest.phase() == Receiver::Phase::loaded && p.guest.Status().phase == GuestPhase::world && p.guest.Status().loaded_load_counter == 1 &&
          p.guest.Status().loaded_sha == p.host.offer().sha);
    // Every message fits one reliable message.
    // The same save offered again (a reconnect after the load): RESULT ok, nothing transferred.
    const std::uint64_t chunks = p.host.chunks_sent;
    p.host.Reannounce();
    p.Run();
    CHECK(p.host.phase() == Sender::Phase::done && p.host.chunks_sent == chunks);
    // A load failure is reported.
    CHECK(p.host.Offer(MakeOffer(2, "autosave3", Reason::in_world_load), save.data(), save.size()));
    p.Run();
    CHECK(p.guest.TakeComplete(got, offer) && offer.reason == Reason::in_world_load);
    p.guest.LoadDone(ResultCode::load_failed);
    p.Run();
    CHECK(p.host.phase() == Sender::Phase::failed && p.host.result() == ResultCode::load_failed && p.guest.phase() == Receiver::Phase::failed);
    // Lower load counters are refused by the sender; equal with other bytes too.
    const Bytes other = MakeCheckpoint(500, 9);
    CHECK(!p.host.Offer(MakeOffer(1), other.data(), other.size()));
    CHECK(!p.host.Offer(MakeOffer(2), other.data(), other.size()));
    CHECK(!p.host.Offer(MakeOffer(3), nullptr, 0));
    Bytes too_big(kMaxSaveBytes + 1, 0);
    CHECK(!p.host.Offer(MakeOffer(3), too_big.data(), too_big.size()));
    Offer badslot = MakeOffer(3);
    badslot.slot_name[0] = 0;
    CHECK(!p.host.Offer(badslot, save.data(), save.size()));
}

void TestResume() {
    Pair p;
    const Bytes save = MakeCheckpoint(9000);
    CHECK(p.host.Offer(MakeOffer(5), save.data(), save.size()));
    // OFFER -> WANT 0 -> three chunks, then the link drops (everything in flight lost).
    Deliver(p.guest, Drain(p.host, 1));
    Deliver(p.host, Drain(p.guest));
    CHECK(p.host.phase() == Sender::Phase::streaming);
    Deliver(p.guest, Drain(p.host, 3));
    CHECK(p.guest.have_bytes() == 3 * kMaxChunk && p.guest.Status().have_bytes == 3 * kMaxChunk);
    (void)Drain(p.host, 2);   // lost on the old session
    // New link session: the host offers again; the guest resumes at 3000.
    p.host.Reannounce();
    const auto offer = Drain(p.host, 1);
    CHECK(offer.size() == 1 && KindOf(offer[0]) == Kind::offer);
    Deliver(p.guest, offer);
    const auto want = Drain(p.guest);
    Message m;
    CHECK(want.size() == 1 && Decode(want[0].data(), want[0].size(), m) && m.h.kind == Kind::want && m.want.next_offset == 3 * kMaxChunk);
    Deliver(p.host, want);
    CHECK(p.host.send_offset() == 3 * kMaxChunk && p.host.resumes == 1);
    p.Run();
    CHECK(p.guest.phase() == Receiver::Phase::complete && p.store.bytes == save && p.guest.resumes >= 1);
    // Chunks sent before the WANT (another offset) are dropped, not appended.
    Pair q;
    CHECK(q.host.Offer(MakeOffer(1), save.data(), save.size()));
    Deliver(q.guest, Drain(q.host, 1));
    Deliver(q.host, Drain(q.guest));
    auto chunks = Drain(q.host, 2);
    Deliver(q.guest, {chunks[1]});   // offset 1000 before 0
    CHECK(q.guest.dropped_chunks == 1 && q.guest.have_bytes() == 0);
    Deliver(q.guest, {chunks[0], chunks[1]});
    CHECK(q.guest.have_bytes() == 2 * kMaxChunk);
    // A guest whose store already holds the start of the same file (incoming\<sha>.part) resumes at once.
    Pair r;
    r.store.open = true;
    r.store.sha = Sha256Of(save.data(), save.size());
    r.store.bytes.assign(save.begin(), save.begin() + 2500);
    CHECK(r.host.Offer(MakeOffer(1), save.data(), save.size()));
    Deliver(r.guest, Drain(r.host));
    const auto w2 = Drain(r.guest);
    CHECK(w2.size() == 1 && Decode(w2[0].data(), w2[0].size(), m) && m.want.next_offset == 2500);
    Deliver(r.host, w2);
    r.Run();
    CHECK(r.guest.phase() == Receiver::Phase::complete && r.store.bytes == save);
    // A WANT past the size or with another SHA: the sender aborts (protocol).
    Pair s;
    CHECK(s.host.Offer(MakeOffer(1), save.data(), save.size()));
    (void)Drain(s.host);
    Message want_bad{};
    want_bad.h.kind = Kind::want;
    want_bad.h.xfer_id = s.host.xfer_id();
    want_bad.want.next_offset = static_cast<std::uint32_t>(save.size() + 1);
    want_bad.want.sha = s.host.offer().sha;
    s.host.OnMessage(want_bad);
    CHECK(s.host.phase() == Sender::Phase::aborted && s.host.abort_code() == AbortCode::protocol);
    const auto ab = Drain(s.host);
    CHECK(ab.size() == 1 && KindOf(ab[0]) == Kind::abort);
    // Messages for another transfer are stale.
    want_bad.h.xfer_id = 999;
    s.host.OnMessage(want_bad);
    CHECK(s.host.stale == 1);
}

void TestSupersede() {
    Pair p;
    const Bytes first = MakeCheckpoint(6000, 1), second = MakeCheckpoint(2000, 2);
    CHECK(p.host.Offer(MakeOffer(1), first.data(), first.size()));
    Deliver(p.guest, Drain(p.host, 1));
    Deliver(p.host, Drain(p.guest));
    Deliver(p.guest, Drain(p.host, 2));
    CHECK(p.guest.phase() == Receiver::Phase::receiving && p.guest.have_bytes() == 2 * kMaxChunk);
    const std::uint32_t old_xfer = p.host.xfer_id();
    const unsigned removes = p.store.removes;
    // The host loads another save: ABORT superseded for the old transfer, then the new OFFER.
    CHECK(p.host.Offer(MakeOffer(2, "autosave1", Reason::in_world_load), second.data(), second.size()));
    CHECK(p.host.xfer_id() != old_xfer);
    const auto msgs = Drain(p.host, 2);
    Message m;
    CHECK(msgs.size() == 2 && Decode(msgs[0].data(), msgs[0].size(), m) && m.h.kind == Kind::abort && m.h.xfer_id == old_xfer &&
          m.abort.code == AbortCode::superseded);
    CHECK(KindOf(msgs[1]) == Kind::offer);
    Deliver(p.guest, msgs);
    CHECK(p.store.removes > removes);   // the old part is gone
    p.Run();
    CHECK(p.guest.phase() == Receiver::Phase::complete && p.store.bytes == second && p.guest.offer().load_counter == 2);
    // A new OFFER with a higher load counter supersedes on the guest even without the ABORT (it was lost).
    Pair q;
    CHECK(q.host.Offer(MakeOffer(1), first.data(), first.size()));
    Deliver(q.guest, Drain(q.host, 1));
    Deliver(q.host, Drain(q.guest));
    Deliver(q.guest, Drain(q.host, 1));
    CHECK(q.guest.have_bytes() == kMaxChunk);
    CHECK(q.host.Offer(MakeOffer(3), second.data(), second.size()));
    auto both = Drain(q.host, 2);
    Deliver(q.guest, {both[1]});   // the ABORT lost: only the OFFER
    CHECK(q.guest.offer().load_counter == 3 && q.guest.have_bytes() == 0);
    // An older offer (lower load counter) arriving late is ignored.
    Offer old = MakeOffer(2);
    old.size = static_cast<std::uint32_t>(first.size());
    old.sha = Sha256Of(first.data(), first.size());
    std::uint8_t buf[kMaxMessage];
    Message om;
    CHECK(Decode(buf, EncodeOffer(77, old, buf, sizeof(buf)), om));
    q.guest.OnMessage(om);
    CHECK(q.guest.stale == 1 && q.guest.offer().load_counter == 3);
    Deliver(q.host, Drain(q.guest));
    q.Run();
    CHECK(q.guest.phase() == Receiver::Phase::complete && q.store.bytes == second);
    // A newer offer while the game loads the previous one: both RESULTs reach the host side; the newer keeps running.
    Bytes got;
    Offer o;
    CHECK(q.guest.TakeComplete(got, o));
    CHECK(q.host.Offer(MakeOffer(4), first.data(), first.size()));
    Deliver(q.guest, Drain(q.host));
    CHECK(q.guest.phase() == Receiver::Phase::receiving);
    q.guest.LoadDone(ResultCode::ok);
    CHECK(q.guest.phase() == Receiver::Phase::receiving && q.guest.Status().loaded_load_counter == 3);
    q.Run();
    CHECK(q.guest.phase() == Receiver::Phase::complete && q.store.bytes == first);
    // The newer save COMPLETES while the game still loads the older one: it waits (complete) for that load's LoadDone;
    // the RESULT names the transfer really loaded (the host's newer transfer is not credited), then the newer is taken.
    {
        Pair t;
        CHECK(t.host.Offer(MakeOffer(1), first.data(), first.size()));
        t.Run();
        const std::uint32_t xfer1 = t.host.xfer_id();
        CHECK(t.guest.TakeComplete(got, o) && o.load_counter == 1 && t.guest.loading());
        CHECK(t.host.Offer(MakeOffer(2, "autosave1", Reason::in_world_load), second.data(), second.size()));
        t.Run();
        CHECK(t.guest.phase() == Receiver::Phase::complete && t.store.bytes == second && t.host.phase() == Sender::Phase::ended);
        CHECK(!t.guest.TakeComplete(got, o) && t.guest.loading());   // the first load still runs
        t.guest.LoadDone(ResultCode::ok);                          // load #1 reached the world
        const auto up = Drain(t.guest);
        CHECK(up.size() == 1 && Decode(up[0].data(), up[0].size(), m) && m.h.kind == Kind::result && m.h.xfer_id == xfer1 && m.result.ok);
        const std::uint64_t stale = t.host.stale;
        Deliver(t.host, up);
        CHECK(t.host.phase() == Sender::Phase::ended && t.host.stale == stale + 1);   // never "done" for #2
        CHECK(t.guest.Status().loaded_load_counter == 1 && t.guest.phase() == Receiver::Phase::complete);
        CHECK(t.guest.TakeComplete(got, o) && o.load_counter == 2 && got == second);
        t.guest.LoadDone(ResultCode::load_failed);   // #2's own result reaches the host
        t.Run();
        CHECK(t.host.phase() == Sender::Phase::failed && t.host.result() == ResultCode::load_failed && t.guest.Status().loaded_load_counter == 1);
    }
    // The host cancels: ABORT, the guest drops its part and waits.
    Pair r;
    CHECK(r.host.Offer(MakeOffer(1), first.data(), first.size()));
    Deliver(r.guest, Drain(r.host, 1));
    Deliver(r.host, Drain(r.guest));
    Deliver(r.guest, Drain(r.host, 1));
    r.host.Cancel(AbortCode::cancelled);
    CHECK(r.host.phase() == Sender::Phase::aborted);
    Deliver(r.guest, Drain(r.host));
    CHECK(r.guest.phase() == Receiver::Phase::waiting && !r.store.open);
}

// A new link generation (Receiver::NewSession): a restarted host counts its loads and transfers from 1 again.
void TestNewSession() {
    const Bytes a = MakeCheckpoint(6000, 1), b = MakeCheckpoint(3000, 2);
    Message m;
    {
        Pair p;
        p.host.set_next_xfer_id(40);
        CHECK(p.host.Offer(MakeOffer(3), a.data(), a.size()));
        Deliver(p.guest, Drain(p.host, 1));
        Deliver(p.host, Drain(p.guest));
        Deliver(p.guest, Drain(p.host, 2));
        CHECK(p.guest.phase() == Receiver::Phase::receiving && p.guest.offer().load_counter == 3);
        // The host's game crashed mid-transfer and started again: load #1, xfer 1.
        Sender fresh;
        CHECK(fresh.Offer(MakeOffer(1), b.data(), b.size()));
        Deliver(p.guest, Drain(fresh, 1));
        CHECK(p.guest.stale == 1 && Drain(p.guest).empty());   // without a new session: dropped as an older offer
        p.guest.NewSession();
        CHECK(p.guest.phase() == Receiver::Phase::waiting && p.guest.Status().phase == GuestPhase::waiting && !p.guest.loading());
        fresh.Reannounce();
        Deliver(p.guest, Drain(fresh, 1));
        CHECK(p.guest.phase() == Receiver::Phase::receiving && p.guest.offer().load_counter == 1 && p.guest.have_bytes() == 0);
        for (int i = 0; i < 100; ++i) {
            const auto down = Drain(fresh);
            Deliver(p.guest, down);
            const auto up = Drain(p.guest);
            Deliver(fresh, up);
            if (down.empty() && up.empty()) break;
        }
        CHECK(p.guest.phase() == Receiver::Phase::complete && p.store.bytes == b);
    }
    {
        // The same host on a new link session: the same file offered again resumes from the part's bytes.
        Pair q;
        CHECK(q.host.Offer(MakeOffer(2), a.data(), a.size()));
        Deliver(q.guest, Drain(q.host, 1));
        Deliver(q.host, Drain(q.guest));
        Deliver(q.guest, Drain(q.host, 3));
        q.guest.NewSession();
        q.host.Reannounce();
        Deliver(q.guest, Drain(q.host, 1));
        const auto w = Drain(q.guest);
        CHECK(w.size() == 1 && Decode(w[0].data(), w[0].size(), m) && m.h.kind == Kind::want && m.want.next_offset == 3 * kMaxChunk);
        Deliver(q.host, w);
        q.Run();
        CHECK(q.guest.phase() == Receiver::Phase::complete && q.store.bytes == a);
    }
    {
        // A load the game is running keeps its RESULT across the new session; the loaded world stays.
        Pair r;
        CHECK(r.host.Offer(MakeOffer(1), a.data(), a.size()));
        r.Run();
        Bytes got;
        Offer o;
        CHECK(r.guest.TakeComplete(got, o));
        r.guest.NewSession();
        CHECK(r.guest.loading() && r.guest.Status().phase == GuestPhase::loading);
        r.guest.LoadDone(ResultCode::ok);
        r.Run();
        CHECK(r.host.phase() == Sender::Phase::done && r.guest.phase() == Receiver::Phase::loaded);
        r.guest.NewSession();
        CHECK(r.guest.phase() == Receiver::Phase::loaded && r.guest.Status().loaded_load_counter == 1);
    }
}

// The SHA-256 check at END: the part is deleted and asked for again at most twice, then RESULT 1.
void TestShaMismatch() {
    const Bytes save = MakeCheckpoint(2500);
    // One corruption: the retry succeeds.
    {
        Pair p;
        CHECK(p.host.Offer(MakeOffer(1), save.data(), save.size()));
        Deliver(p.guest, Drain(p.host, 1));
        Deliver(p.host, Drain(p.guest));
        auto stream = Drain(p.host);   // 3 chunks + END
        CHECK(stream.size() == 4 && KindOf(stream.back()) == Kind::end);
        Deliver(p.guest, {stream[0], stream[1], stream[2]});
        p.store.bytes[100] ^= 0x55;   // the disk changed a byte
        Deliver(p.guest, {stream[3]});
        CHECK(p.guest.sha_retries == 1 && p.guest.phase() == Receiver::Phase::receiving && p.guest.have_bytes() == 0);
        const auto want = Drain(p.guest);
        Message m;
        CHECK(want.size() == 1 && Decode(want[0].data(), want[0].size(), m) && m.h.kind == Kind::want && m.want.next_offset == 0);
        Deliver(p.host, want);
        CHECK(p.host.restarts == 1);
        p.Run();
        CHECK(p.guest.phase() == Receiver::Phase::complete && p.store.bytes == save);
    }
    // Always corrupt: two retries, then RESULT sha_mismatch and the guest waits.
    {
        Pair p;
        CHECK(p.host.Offer(MakeOffer(1), save.data(), save.size()));
        Deliver(p.guest, Drain(p.host, 1));
        Deliver(p.host, Drain(p.guest));
        unsigned wants = 0;
        for (int round = 0; round < 5; ++round) {
            auto stream = Drain(p.host);
            if (stream.empty()) break;
            for (const Bytes& b : stream) {
                if (KindOf(b) == Kind::end && p.store.bytes.size() > 10) p.store.bytes[5] ^= 1;
                Deliver(p.guest, {b});
            }
            const auto up = Drain(p.guest);
            for (const Bytes& b : up) wants += KindOf(b) == Kind::want;
            Deliver(p.host, up);
        }
        CHECK(wants == kMaxShaRetries);
        CHECK(p.guest.sha_retries == kMaxShaRetries + 1 && p.guest.phase() == Receiver::Phase::failed &&
              p.guest.Status().phase == GuestPhase::waiting && p.guest.last_error() == ResultCode::sha_mismatch);
        CHECK(p.host.phase() == Sender::Phase::failed && p.host.result() == ResultCode::sha_mismatch);
        CHECK(!p.store.open);   // nothing left behind
    }
    // An END whose size / SHA disagrees with the OFFER: protocol abort.
    {
        Pair p;
        CHECK(p.host.Offer(MakeOffer(1), save.data(), save.size()));
        Deliver(p.guest, Drain(p.host, 1));
        Deliver(p.host, Drain(p.guest));
        End e;
        e.size = static_cast<std::uint32_t>(save.size() + 1);
        e.sha = p.host.offer().sha;
        std::uint8_t buf[kMaxMessage];
        Message m;
        CHECK(Decode(buf, EncodeEnd(p.host.xfer_id(), e, buf, sizeof(buf)), m));
        p.guest.OnMessage(m);
        const auto up = Drain(p.guest);
        CHECK(p.guest.phase() == Receiver::Phase::failed && up.size() == 1 && KindOf(up[0]) == Kind::abort);
    }
    // A save that is not a checkpoint.dat (SHA fine): RESULT parse.
    {
        Pair p;
        Bytes junk(3000, 0x42);
        CHECK(p.host.Offer(MakeOffer(1), junk.data(), junk.size()));
        p.Run();
        CHECK(p.guest.phase() == Receiver::Phase::failed && p.guest.last_error() == ResultCode::parse && p.host.result() == ResultCode::parse);
    }
    // A disk error while appending: RESULT disk.
    {
        Pair p;
        p.store.fail_append = true;
        CHECK(p.host.Offer(MakeOffer(1), save.data(), save.size()));
        p.Run();
        CHECK(p.guest.phase() == Receiver::Phase::failed && p.host.result() == ResultCode::disk);
    }
}

// Another game build or save version: refused, nothing stored, both sides know.
void TestVersionRefusal() {
    Pair p;
    p.guest.SetLocalVersion(Version("dsq 180/4100000"));
    const Bytes save = MakeCheckpoint(1000);
    State s;
    s.version = Version();
    std::uint8_t buf[kMaxMessage];
    Message m;
    CHECK(Decode(buf, EncodeState(s, buf, sizeof(buf)), m));
    p.guest.OnMessage(m);
    CHECK(p.guest.version_mismatch() && p.guest.phase() == Receiver::Phase::refused && p.guest.Status().phase == GuestPhase::refused &&
          p.guest.Status().refuse_reason == static_cast<std::uint16_t>(ResultCode::version));
    CHECK(p.host.Offer(MakeOffer(1), save.data(), save.size()));
    p.Run();
    CHECK(!p.store.open && p.store.bytes.empty());   // no file written
    CHECK(p.host.phase() == Sender::Phase::failed && p.host.result() == ResultCode::version);
    // Save version and flags count too.
    Pair q;
    q.guest.SetLocalVersion(Version("dsq 179/4027081", 68));
    CHECK(q.host.Offer(MakeOffer(1), save.data(), save.size()));
    q.Run();
    CHECK(q.host.result() == ResultCode::version && !q.store.open);
    Pair r;
    r.guest.SetLocalVersion(Version("dsq 179/4027081", 67, 1));
    CHECK(r.host.Offer(MakeOffer(1), save.data(), save.size()));
    r.Run();
    CHECK(r.host.result() == ResultCode::version);
    // A guest that never set its own version refuses too (it cannot know).
    MemoryPartStore st;
    Receiver unset(st);
    Sender h;
    CHECK(h.Offer(MakeOffer(1), save.data(), save.size()));
    Deliver(unset, Drain(h));
    Deliver(h, Drain(unset));
    CHECK(h.result() == ResultCode::version && !st.open);
}

// The link's per-tick cap for reliable packets (link_protocol.hpp).
void TestBurstCap() {
    namespace lk = stcoop::link;
    static_assert(lk::kSaveChannel == kChannel && lk::kSaveChannel < lk::kReliableChannels, "channel 4 is the save channel");
    CHECK(lk::ReliablePacketsPerTick(false, false) == 4);
    CHECK(lk::ReliablePacketsPerTick(true, false) == 4);    // in the world: the poses come first
    CHECK(lk::ReliablePacketsPerTick(false, true) == 4);    // nothing queued
    CHECK(lk::ReliablePacketsPerTick(true, true) == 16);    // title / loading with save data queued
    // The largest message fits the reliable channel and the link body.
    static_assert(kMaxMessage == lk::kMaxReliableMessage && kHeaderSize + kChunkHead + kMaxChunk <= lk::kMaxReliableMessage, "a CHUNK fits");
}

// ---- Windows wrappers on a temporary folder ----------------------------------------------------------------------
std::wstring TempDir() {
    wchar_t base[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, base);
    std::wstring dir = std::wstring(base) + L"st-save-xfer-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

bool WriteRaw(const std::wstring& path, const Bytes& b) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    const bool ok = WriteFile(h, b.data(), static_cast<DWORD>(b.size()), &w, nullptr) && w == b.size();
    CloseHandle(h);
    return ok;
}

void RemoveTreeW(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            const std::wstring p = dir + L"\\" + n;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveTreeW(p);
            else DeleteFileW(p.c_str());
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    RemoveDirectoryW(dir.c_str());
}

void TestReadOnlyReader() {
    // The assertion: GENERIC_READ with all three share flags, nothing else.
    CHECK(OpenAccessAllowed(GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE));
    for (unsigned long access : {static_cast<unsigned long>(GENERIC_WRITE), static_cast<unsigned long>(GENERIC_READ | GENERIC_WRITE),
                                 static_cast<unsigned long>(GENERIC_ALL), static_cast<unsigned long>(FILE_WRITE_DATA),
                                 static_cast<unsigned long>(FILE_APPEND_DATA), static_cast<unsigned long>(DELETE),
                                 static_cast<unsigned long>(GENERIC_READ | DELETE), static_cast<unsigned long>(FILE_GENERIC_READ),
                                 static_cast<unsigned long>(MAXIMUM_ALLOWED), static_cast<unsigned long>(WRITE_DAC), 0ul})
        CHECK(!OpenAccessAllowed(access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE));
    for (unsigned long share : {0ul, static_cast<unsigned long>(FILE_SHARE_READ), static_cast<unsigned long>(FILE_SHARE_READ | FILE_SHARE_WRITE),
                                static_cast<unsigned long>(FILE_SHARE_READ | FILE_SHARE_DELETE)})
        CHECK(!OpenAccessAllowed(GENERIC_READ, share));

    const std::wstring dir = TempDir();
    const std::wstring file = dir + L"\\checkpoint.dat";
    const Bytes save = MakeCheckpoint(5000);
    CHECK(WriteRaw(file, save));
    Bytes got;
    CHECK(ReadSaveFileReadOnly(file, got) == ReadStatus::ok && got == save);
    CHECK(ReadSaveSnapshot(file, got, 10, 2) == ReadStatus::ok && got == save);
    CHECK(ReadSaveFileReadOnly(dir + L"\\missing.dat", got) == ReadStatus::open_failed && got.empty());
    CHECK(ReadSaveFileReadOnly(file, got, 100) == ReadStatus::too_large);
    {
        // While a read handle with our share flags is open, another writer (the game) still opens the file for writing
        // and for delete: we never block it. (A rename OVER an open file fails on Windows whatever the share flags, so
        // the reader holds its handle only for the one read: ReadSaveFileReadOnly opens, reads and closes.)
        HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        CHECK(h != INVALID_HANDLE_VALUE);
        HANDLE w = CreateFileW(file.c_str(), GENERIC_WRITE | DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0,
                               nullptr);
        CHECK(w != INVALID_HANDLE_VALUE);
        if (w != INVALID_HANDLE_VALUE) CloseHandle(w);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        // After a read nothing is held: the game's temp + rename goes through.
        CHECK(ReadSaveFileReadOnly(file, got) == ReadStatus::ok);
        const std::wstring temp = dir + L"\\checkpoint.dattemp";
        CHECK(WriteRaw(temp, MakeCheckpoint(10)));
        CHECK(MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING) != 0);
        // While the game's checkpoint.dattemp sits next to the slot (the game is writing it), the reader opens nothing.
        CHECK(WriteRaw(temp, MakeCheckpoint(20)));
        CHECK(ReadSaveFileReadOnly(file, got) == ReadStatus::busy && got.empty());
        CHECK(ReadSaveSnapshot(file, got, 5, 2) == ReadStatus::busy && got.empty());
        CHECK(DeleteFileW(temp.c_str()) != 0);
    }
    // The reader never changed the file: what it reads is what was written.
    CHECK(ReadSaveFileReadOnly(file, got) == ReadStatus::ok && got == MakeCheckpoint(10));
    RemoveTreeW(dir);
}

void TestSandboxWriter() {
    const std::wstring dir = TempDir();
    const std::wstring scratch = dir + L"\\sandbox\\coop-guest\\saves";
    const std::wstring real = dir + L"\\real\\KojimaProductions";
    CreateDirectoryW((dir + L"\\sandbox").c_str(), nullptr);
    CreateDirectoryW((dir + L"\\sandbox\\coop-guest").c_str(), nullptr);
    CreateDirectoryW(scratch.c_str(), nullptr);
    CreateDirectoryW((dir + L"\\real").c_str(), nullptr);
    CreateDirectoryW(real.c_str(), nullptr);
    SandboxTarget t;
    t.scratch_root = scratch;
    t.redirected = true;
    t.real_roots = {real};
    CHECK(SandboxTargetProblem(t).empty());
    const Bytes save = MakeCheckpoint(3000);
    const std::wstring rel = ManualSlotRel(104729, 5);
    std::string why;
    CHECK(WriteGuestSaveFile(t, rel, save.data(), save.size(), &why));
    Bytes got;
    CHECK(ReadSaveFileReadOnly(scratch + L"\\" + rel, got) == ReadStatus::ok && got == save);
    CHECK(GetFileAttributesW((scratch + L"\\" + rel + L".st-tmp").c_str()) == INVALID_FILE_ATTRIBUTES);   // no temp left
    // Replacing works (slot 5 again with another save).
    const Bytes save2 = MakeCheckpoint(100, 3);
    CHECK(WriteGuestSaveFile(t, rel, save2.data(), save2.size(), &why) && ReadSaveFileReadOnly(scratch + L"\\" + rel, got) == ReadStatus::ok &&
          got == save2);
    // Refusals: not redirected, no root, a root overlapping a real root, paths outside the two allowed kinds.
    SandboxTarget off = t;
    off.redirected = false;
    CHECK(!WriteGuestSaveFile(off, rel, save.data(), save.size(), &why) && why.find("redirect") != std::string::npos);
    SandboxTarget none = t;
    none.scratch_root.clear();
    CHECK(!WriteGuestSaveFile(none, rel, save.data(), save.size(), &why));
    SandboxTarget inside_real = t;
    inside_real.scratch_root = real + L"\\DeathStrandingDC";
    CreateDirectoryW(inside_real.scratch_root.c_str(), nullptr);
    CHECK(!SandboxTargetProblem(inside_real).empty() && !WriteGuestSaveFile(inside_real, rel, save.data(), save.size(), &why));
    SandboxTarget real_is_scratch = t;
    real_is_scratch.real_roots = {scratch};
    CHECK(!WriteGuestSaveFile(real_is_scratch, rel, save.data(), save.size(), &why));
    SandboxTarget relative = t;
    relative.scratch_root = L"sandbox\\coop-guest\\saves";
    CHECK(!WriteGuestSaveFile(relative, rel, save.data(), save.size(), &why));
    for (const wchar_t* bad : {L"DeathStrandingDC\\104729\\autosave0\\checkpoint.dat", L"..\\..\\real\\KojimaProductions\\x.dat",
                               L"DeathStrandingDC\\104729\\profile\\profile.dat"})
        CHECK(!WriteGuestSaveFile(t, bad, save.data(), save.size(), &why));
    CHECK(!WriteGuestSaveFile(t, rel, save.data(), 0, &why) && !WriteGuestSaveFile(t, rel, nullptr, 10, &why));
    // The real root stayed empty.
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((real + L"\\*").c_str(), &fd);
    int entries = 0;
    if (f != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring n = fd.cFileName;
            if (n != L"." && n != L"..") ++entries;
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    CHECK(entries == 1);   // only the DeathStrandingDC folder this test made itself

    // The part store on disk, driven by a real transfer.
    {
        SandboxPartStore store(t);
        Receiver guest(store);
        guest.SetLocalVersion(Version());
        Sender host;
        const Bytes big = MakeCheckpoint(7777);
        CHECK(host.Offer(MakeOffer(1), big.data(), big.size()));
        Deliver(guest, Drain(host, 1));
        Deliver(host, Drain(guest));
        Deliver(guest, Drain(host, 4));
        const std::wstring part = scratch + L"\\" + IncomingPartRel(host.offer().sha);
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        CHECK(GetFileAttributesExW(part.c_str(), GetFileExInfoStandard, &fa) && fa.nFileSizeLow == 4 * kMaxChunk);
        // A new receiver (the guest's mod after a reconnect keeps the part): resume from the file.
        SandboxPartStore store2(t);
        Receiver guest2(store2);
        guest2.SetLocalVersion(Version());
        host.Reannounce();
        Deliver(guest2, Drain(host));
        const auto want = Drain(guest2);
        Message m;
        CHECK(want.size() == 1 && Decode(want[0].data(), want[0].size(), m) && m.want.next_offset == 4 * kMaxChunk);
        Deliver(host, want);
        for (int i = 0; i < 50; ++i) {
            const auto down = Drain(host);
            Deliver(guest2, down);
            const auto up = Drain(guest2);
            Deliver(host, up);
            if (down.empty() && up.empty()) break;
        }
        Bytes data;
        Offer o;
        CHECK(guest2.phase() == Receiver::Phase::complete && guest2.TakeComplete(data, o) && data == big);
        CHECK(GetFileAttributesW(part.c_str()) == INVALID_FILE_ATTRIBUTES);   // removed once handed over
        // Then written as the slot the game loads.
        CHECK(WriteGuestSaveFile(t, ManualSlotRel(104729, 6), data.data(), data.size(), &why));
        // Not redirected: the store refuses to open (RESULT disk), nothing written.
        SandboxPartStore off_store(off);
        Receiver guest3(off_store);
        guest3.SetLocalVersion(Version());
        Sender h3;
        CHECK(h3.Offer(MakeOffer(1), big.data(), big.size()));
        Deliver(guest3, Drain(h3));
        Deliver(h3, Drain(guest3));
        CHECK(h3.result() == ResultCode::disk && !off_store.last_error().empty());
    }
    RemoveTreeW(dir);
}

// The game side's decision rules (coop_rules.hpp; docs/coop-session.md section 5).
void TestTitleLabel() {
    namespace r = st::coop::rules;
    CHECK(r::SafeHostName("player") == "player");
    CHECK(r::SafeHostName("  player  ") == "player");
    CHECK(r::SafeHostName("") == "the host" && r::SafeHostName(nullptr) == "the host" && r::SafeHostName("   ") == "the host");
    CHECK(r::SafeHostName("pl\xC3\xA4yer") == "the host");   // non-ASCII: the game font may have no glyph
    CHECK(r::SafeHostName("a-very-long-player-name-here") == "a-very-long-p...");
    r::TitleLabelIn in;
    in.host = "player";
    CHECK(r::TitleLabel(in) == "Connecting to player...");
    in.ever_connected = true;
    CHECK(r::TitleLabel(in) == "Reconnecting to player...");
    in.connected = true;
    CHECK(r::TitleLabel(in) == "Waiting for player...");
    in.no_reply = true;
    CHECK(r::TitleLabel(in) == "No reply from player's save session");
    in.no_reply = false;
    in.have_host_state = true;
    CHECK(r::TitleLabel(in) == "Waiting for player to start the game");
    in.host_phase = st::stx::HostPhase::loading;
    CHECK(r::TitleLabel(in) == "Waiting while player loads the game");
    in.host_phase = st::stx::HostPhase::world;
    CHECK(r::TitleLabel(in) == "Joining player's game...");
    in.new_game_pending = true;
    CHECK(r::TitleLabel(in) == "Waiting for player's first save");
    in.receiving = true;
    in.have = 450;
    in.size = 1000;
    CHECK(r::TitleLabel(in) == "Receiving player's save: 45%");
    in.have = 5000;   // never above 100%
    CHECK(r::TitleLabel(in) == "Receiving player's save: 100%");
    in.connected = false;   // the link first
    CHECK(r::TitleLabel(in) == "Reconnecting to player...");
    in.loading = true;      // our own load beats the link
    CHECK(r::TitleLabel(in) == "Loading player's save...");
    in.version_mismatch = true;
    CHECK(r::TitleLabel(in) == "Game versions differ: update both games");
    // Every label fits the menu item (at most 46 characters with a 16-character name).
    r::TitleLabelIn lg;
    lg.host = r::SafeHostName("a-very-long-player-name-here");
    lg.connected = lg.ever_connected = lg.have_host_state = true;
    CHECK(r::TitleLabel(lg).size() <= 46);
    lg.connected = false;
    CHECK(r::TitleLabel(lg).size() <= 46);
    lg.connected = lg.receiving = true;
    lg.size = lg.have = 1;
    CHECK(r::TitleLabel(lg).size() <= 46);
    CHECK(r::TitleItemHidden("itm_config_menu") && r::TitleItemHidden("itm_update_info") && r::TitleItemHidden("itm_credit_menu"));
    CHECK(r::TitleItemHidden("itm_something_new"));
    CHECK(!r::TitleItemHidden("itm_new_game") && !r::TitleItemHidden("itm_exit_game_menu") && !r::TitleItemHidden("itm_continue") &&
          !r::TitleItemHidden("itm_load_game") && !r::TitleItemHidden("itm_load_game_PS4_to_PS5") && !r::TitleItemHidden(""));
}

void TestCoopRules() {
    namespace r = st::coop::rules;
    // The -2 follow-up (run 20261119-065746: 0.7 s after the title Continue, before a stable player) is not a Retry.
    CHECK(r::IsFollowUp(false, 1700, 1000, false, 0xF, -2, 0xF, -1));
    CHECK(r::IsFollowUp(true, 999999, 0, true, 4, -2, 0xF, -1));                 // no session: always part of a load
    CHECK(!r::IsFollowUp(false, 1000 + r::kFollowUpMs + 1, 1000, false, 0xF, -2, 0xF, -1));   // too late: a Retry
    CHECK(!r::IsFollowUp(false, 2000, 1000, true, 4, -2, 0xF, -1));             // a stable player since: a Retry
    CHECK(r::IsFollowUp(false, 2000, 1000, false, 4, 18, 4, 18));               // the same in-world Load again
    CHECK(!r::IsFollowUp(false, 2000, 1000, false, 4, 17, 4, 18));              // another slot: a new Load
    // Phases.
    CHECK(r::HostPhaseOf(false, false, false) == HostPhase::title);
    CHECK(r::HostPhaseOf(false, false, true) == HostPhase::loading);
    CHECK(r::HostPhaseOf(true, false, false) == HostPhase::loading);   // a session without a stable player
    CHECK(r::HostPhaseOf(true, true, true) == HostPhase::loading);     // the load is still pending
    CHECK(r::HostPhaseOf(true, true, false) == HostPhase::world);
    CHECK(r::GuestPhaseOf(GuestPhase::world, true, false) == GuestPhase::waiting);
    CHECK(r::GuestPhaseOf(GuestPhase::world, false, true) == GuestPhase::world);
    CHECK(r::GuestPhaseOf(GuestPhase::receiving, true, false) == GuestPhase::receiving);
    CHECK(r::NextGuestSlot(5) == 6 && r::NextGuestSlot(6) == 5 && r::NextGuestSlot(-1) == 5);
    // Late join.
    r::LateJoinIn in;
    in.connected = in.have_gstate = true;
    in.gstate_age_ms = 500;
    in.host_phase = HostPhase::world;
    in.gstate.phase = GuestPhase::waiting;
    CHECK(r::WantLateJoin(in) == r::LateJoin::waiting_partner);
    in.since_last_offer_ms = r::kReofferMs - 1;
    CHECK(r::WantLateJoin(in) == r::LateJoin::none);                     // at most once per kReofferMs
    in.since_last_offer_ms = 1e18;
    in.sender_busy = true;
    CHECK(r::WantLateJoin(in) == r::LateJoin::none);                     // an offer is running
    in.sender_busy = false;
    in.host_phase = HostPhase::loading;
    CHECK(r::WantLateJoin(in) == r::LateJoin::none);                     // only from the world
    in.host_phase = HostPhase::world;
    in.version_mismatch = true;
    CHECK(r::WantLateJoin(in) == r::LateJoin::none);
    in.version_mismatch = false;
    in.gstate_age_ms = 5000;
    CHECK(r::WantLateJoin(in) == r::LateJoin::none);                     // a stale GSTATE
    in.gstate_age_ms = 500;
    in.new_game_pending = true;
    CHECK(r::WantLateJoin(in) == r::LateJoin::none);                     // New Game: the first save comes instead
    in.new_game_pending = false;
    in.gstate.phase = GuestPhase::refused;
    CHECK(r::WantLateJoin(in) == r::LateJoin::none);
    in.gstate.phase = GuestPhase::loading;
    CHECK(r::WantLateJoin(in) == r::LateJoin::none);
    // In the world: the current one, or an older world.
    in.gstate.phase = GuestPhase::world;
    in.sender_done = true;
    in.current_counter = 3;
    in.current_sha = Sha256Of("x", 1);
    in.gstate.loaded_load_counter = 3;
    in.gstate.loaded_sha = in.current_sha;
    CHECK(r::WantLateJoin(in) == r::LateJoin::none);
    in.gstate.loaded_load_counter = 2;
    CHECK(r::WantLateJoin(in) == r::LateJoin::older_world);
    in.gstate.loaded_load_counter = 3;
    in.sender_done = false;   // the current offer failed or was aborted
    CHECK(r::WantLateJoin(in) == r::LateJoin::older_world);
    // A stale unanswered offer on reconnect.
    CHECK(r::StaleOnReconnect(HostPhase::world, true, Reason::title_continue));
    CHECK(r::StaleOnReconnect(HostPhase::world, true, Reason::late_join));
    CHECK(!r::StaleOnReconnect(HostPhase::world, true, Reason::in_world_load));   // the guest follows that load: resume
    CHECK(!r::StaleOnReconnect(HostPhase::world, false, Reason::title_continue)); // already answered: resume
    CHECK(!r::StaleOnReconnect(HostPhase::loading, true, Reason::title_continue)); // the host still loads it
    // The guest's title load waits for its title menu (boot, and after QuitGame).
    CHECK(!r::TitleReady(5000 + r::kTitleBootMs - 1, 5000, true, 5000));          // boot logos / notices: too early
    CHECK(r::TitleReady(5000 + r::kTitleBootMs, 5000, true, 5000));
    CHECK(!r::TitleReady(100000, 5000, false, 5000));                              // in the world: never the title load
    CHECK(!r::TitleReady(100000, 5000, true, 100000 - r::kTitleSettleMs + 1));     // just back at the title (QuitGame)
    CHECK(r::TitleReady(100000, 5000, true, 100000 - r::kTitleSettleMs));
    // The newest save for the host's current world.
    const std::vector<r::SlotTime> slots = {{"autosave17", 900}, {"manualsave3", 100}, {"autosave2", 50}, {"quicksave0", 950}, {"autosave5", 1200}};
    bool own = true;
    CHECK(r::NewestCandidates(slots, false, 0, "", &own) == std::vector<std::string>({"autosave5", "quicksave0", "autosave17"}) && !own);
    CHECK(r::NewestCandidates(slots, true, 0, "") == std::vector<std::string>({"autosave5", "autosave17", "autosave2"}));
    // The host loaded manualsave3 at t=1000: only saves written since then belong to its world.
    CHECK(r::NewestCandidates(slots, false, 1000, "manualsave3", &own) == std::vector<std::string>({"autosave5"}) && !own);
    // Nothing written since the load (late join right after, or a Retry before the first autosave): the world's own slot,
    // never yesterday's autosave17.
    CHECK(r::NewestCandidates(slots, false, 1300, "manualsave3", &own) == std::vector<std::string>({"manualsave3"}) && own);
    CHECK(r::NewestCandidates(slots, true, 1300, "manualsave3", &own) == std::vector<std::string>({"manualsave3"}) && own);
    CHECK(r::NewestCandidates(slots, true, 1300, "manualsave9", &own).empty() && !own);   // that slot is gone: nothing
    CHECK(r::NewestCandidates(slots, true, 1300, "", &own).empty());
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);   // a crash still shows the FAIL lines before it
    TestSha256();
    TestRoundTrips();
    TestDecoderRejections();
    TestSlotNamesAndPaths();
    TestCheckpoint();
    TestTransfer();
    TestResume();
    TestSupersede();
    TestNewSession();
    TestShaMismatch();
    TestVersionRefusal();
    TestBurstCap();
    TestReadOnlyReader();
    TestSandboxWriter();
    TestCoopRules();
    TestTitleLabel();
    if (g_fail) {
        std::printf("st_save_xfer_tests: %d of %d checks FAILED\n", g_fail, g_checks);
        return 1;
    }
    std::printf("st_save_xfer_tests: all %d checks passed\n", g_checks);
    return 0;
}
