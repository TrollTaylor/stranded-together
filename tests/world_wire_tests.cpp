// SWS1 (native/mod/world_wire.*) and the world sync rules (native/mod/world_rules.hpp), the shared structures' pure part:
// every message round trip and decoder rejection, the Record's ranges against the save writer's field set, the bucket
// hashes and the DIGEST chunking / reassembly, the type table, the binding, the guest build check, the removal classes,
// the online gate's verdict, the OTGC write names and the dedup window. No game, no link.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "world_rules.hpp"
#include "world_wire.hpp"

using namespace st::wwire;
namespace wr = st::wrules;

namespace {
int g_checks = 0, g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        ++g_checks;                                                  \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

Header H(Msg t, std::uint16_t flags = 0, std::uint32_t world = 3, std::uint32_t seq = 7) {
    Header h;
    h.type = static_cast<std::uint8_t>(t);
    h.flags = flags;
    h.host_world = world;
    h.seq = seq;
    return h;
}

// A valid record: a postbox at (100, -200, 30), yaw 90 degrees, level 1.
Record Rec(std::uint8_t type = 6, double x = 100, double y = -200, double z = 30) {
    std::uint8_t info[kInfoSize] = {};
    info[0] = type;
    info[2] = 0xFF, info[3] = 0xFF;
    const double p[3] = {x, y, z};
    std::memcpy(info + 0x08, p, 24);
    const float rot[9] = {0, 1, 0, -1, 0, 0, 0, 0, 1};
    std::memcpy(info + 0x20, rot, 36);
    const std::uint32_t level = 1;
    std::memcpy(info + 0x4C, &level, 4);
    info[0x70] = 2;
    const float dur = 3600;
    std::memcpy(info + 0x7C, &dur, 4);
    const std::int32_t mats[6] = {1, 2, 3, 4, 5, 6};
    std::memcpy(info + 0x90, mats, 24);
    info[0x62E] = 1;
    Record r;
    RecordFromInfo(info, &r);
    return r;
}
Xf PccXf() {
    Xf x;
    x.pos[0] = 95, x.pos[1] = -201, x.pos[2] = 30;
    const float rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    std::memcpy(x.rot, rot, 36);
    return x;
}
Entry Ent(std::uint32_t hid, std::uint8_t type, double x, double y, double z) {
    Entry e;
    e.hid = hid;
    e.type = type;
    e.state = 2;
    e.level = 1;
    e.pos[0] = x, e.pos[1] = y, e.pos[2] = z;
    return e;
}

void TestRecord() {
    static_assert(kRecordSize == 496, "the Record");
    // the ranges never touch the fields that must not cross machines
    for (std::size_t off : {0x48u, 0x50u, 0x54u, 0x5Cu, 0x64u, 0x68u, 0xA8u, 0x118u, 0x2F0u, 0x644u, 0x645u, 0x650u, 0x670u, 0x678u}) CHECK(RecordOffset(off) < 0);
    // and cover every field the save writer (WriteRecord 0x142CA7970) persists
    for (std::size_t off : {0x00u, 0x02u, 0x08u, 0x20u, 0x40u, 0x4Cu, 0x70u, 0x74u, 0x78u, 0x7Cu, 0x80u, 0x84u, 0x88u, 0x8Cu, 0x90u, 0xA4u, 0x2D8u, 0x2ECu,
                            0x520u, 0x528u, 0x540u, 0x554u, 0x558u, 0x570u, 0x574u, 0x578u, 0x5A4u, 0x5A8u, 0x5C0u, 0x5E0u, 0x5E8u, 0x5F8u, 0x5FCu, 0x600u,
                            0x608u, 0x610u, 0x618u, 0x620u, 0x62Eu, 0x643u, 0x680u, 0x6BCu})
        CHECK(RecordOffset(off) >= 0);
    const Record r = Rec();
    CHECK(RecordValid(r));
    CHECK(r.Type() == 6);
    CHECK(r.Level() == 1);
    CHECK(r.State() == 2);
    CHECK(r.Flag(0x62E) == 1);
    double p[3];
    r.Pos(p);
    CHECK(p[0] == 100 && p[1] == -200 && p[2] == 30);
    // Record -> info -> Record keeps everything; info outside the ranges is untouched
    std::uint8_t info[kInfoSize];
    std::memset(info, 0xCD, sizeof(info));
    RecordToInfo(r, info);
    CHECK(info[0x48] == 0xCD && info[0x645] == 0xCD && info[0x670] == 0xCD);
    Record back;
    RecordFromInfo(info, &back);
    CHECK(back == r);
    // invalid records
    Record bad = r;
    bad.b[0] = 47;
    CHECK(!RecordValid(bad));
    bad = r;
    const double far3[3] = {30000, 0, 0};
    bad.SetPos(far3);
    CHECK(!RecordValid(bad));
    bad = r;
    const double nan[3] = {std::numeric_limits<double>::quiet_NaN(), 0, 0};
    bad.SetPos(nan);
    CHECK(!RecordValid(bad));
    bad = r;
    std::memset(bad.b + RecordOffset(0x20), 0, 36);  // no rotation
    CHECK(!RecordValid(bad));
    bad = r;
    const std::uint32_t lvl = 9;
    std::memcpy(bad.b + RecordOffset(0x4C), &lvl, 4);
    CHECK(!RecordValid(bad));
}

void TestRoundTrips() {
    std::uint8_t buf[kMaxMessage];
    {
        Hello b;
        b.on = 1, b.parts = 1, b.anim_mode = 2, b.ready = 1, b.world = 9, b.sender = 0xA5A5F00Du;
        const std::size_t n = EncodeHello(H(Msg::hello), b, buf, sizeof(buf));
        CHECK(n == kHeaderSize + 14);
        Header h;
        Hello o;
        CHECK(DecodeHello(buf, n, &h, &o));
        CHECK(o.on == 1 && o.parts == 1 && o.anim_mode == 2 && o.ready == 1 && o.world == 9 && o.sender == 0xA5A5F00Du && h.seq == 7 && h.host_world == 3);
        CHECK(!DecodeHello(buf, n - 4, &h, &o));  // the old 10-byte body (no sender) is refused
        CHECK(!DecodeSnapReq(buf, n, &h, nullptr));  // wrong type
    }
    {
        SnapReq b;
        b.guest_world = 4, b.reason = 1;
        const std::size_t n = EncodeSnapReq(H(Msg::snap_req), b, buf, sizeof(buf));
        Header h;
        SnapReq o;
        CHECK(n && DecodeSnapReq(buf, n, &h, &o) && o.guest_world == 4 && o.reason == 1);
    }
    {
        Sum b;
        b.count = 12;
        for (int i = 0; i < kBuckets; ++i) b.buckets[i] = 0x1111111111111111ull * static_cast<unsigned>(i % 7);
        const std::size_t n = EncodeSum(H(Msg::sum), b, buf, sizeof(buf));
        Header h;
        Sum o;
        CHECK(n && DecodeSum(buf, n, &h, &o) && o.count == 12 && std::memcmp(o.buckets, b.buckets, sizeof(o.buckets)) == 0);
    }
    {
        BucketReq b;
        b.mask = 0x8000000000000001ull;
        const std::size_t n = EncodeBucketReq(H(Msg::bucket_req), b, buf, sizeof(buf));
        Header h;
        BucketReq o;
        CHECK(n && DecodeBucketReq(buf, n, &h, &o) && o.mask == b.mask);
        b.mask = 0;
        CHECK(EncodeBucketReq(H(Msg::bucket_req), b, buf, sizeof(buf)) == 0);
    }
    {
        std::vector<Entry> all;
        for (std::uint32_t i = 0; i < 60; ++i) all.push_back(Ent(10001 + i * 64, 6, i * 10.0, 5, 1));  // all in bucket 17
        const std::uint64_t mask = 1ull << BucketOf(10001);
        const auto parts = ChunkDigest(all, mask, 5);
        CHECK(parts.size() == 3);
        DigestAssembler as;
        // out of order, with a repeat
        for (int k : {2, 0, 0, 1}) {
            const std::size_t n = EncodeDigest(H(Msg::digest), parts[static_cast<std::size_t>(k)], buf, sizeof(buf));
            CHECK(n > 0 && n <= kMaxMessage);
            Header h;
            Digest d;
            CHECK(DecodeDigest(buf, n, &h, &d));
            as.Add(d);
        }
        CHECK(as.Complete());
        CHECK(as.Entries().size() == 60);
        CHECK(as.Mask() == mask);
        // an empty answer still comes as one part
        const auto none = ChunkDigest(all, 1ull << 3, 6);
        CHECK(none.size() == 1 && none[0].entries.empty());
        // an entry outside the answered buckets is refused
        Digest d = parts[0];
        d.mask = 1ull << 3;
        const std::size_t n = EncodeDigest(H(Msg::digest), d, buf, sizeof(buf));
        Header h;
        Digest o;
        CHECK(n && !DecodeDigest(buf, n, &h, &o));
    }
    {
        Need b;
        b.hids = {10001, 10002, 10099};
        const std::size_t n = EncodeNeed(H(Msg::need), b, buf, sizeof(buf));
        Header h;
        Need o;
        CHECK(n && DecodeNeed(buf, n, &h, &o) && o.hids == b.hids);
        b.hids.clear();
        CHECK(EncodeNeed(H(Msg::need), b, buf, sizeof(buf)) == 0);
    }
    {
        Add b;
        b.hid = 10501;
        b.origin = Origin::guest;
        b.req.nonce = 0xABCD, b.req.n = 3;
        b.has_pcc = true;
        b.pcc = PccXf();
        b.rec = Rec();
        const std::size_t n = EncodeAdd(H(Msg::add, kFlagAnim), b, buf, sizeof(buf));
        CHECK(n > 0 && n <= kMaxMessage);
        Header h;
        Add o;
        CHECK(DecodeAdd(buf, n, &h, &o));
        CHECK(o.hid == 10501 && o.origin == Origin::guest && o.req == b.req && o.has_pcc && o.rec == b.rec && (h.flags & kFlagAnim));
        CHECK(std::memcmp(o.pcc.pos, b.pcc.pos, 24) == 0);
        b.has_pcc = false;
        const std::size_t n2 = EncodeAdd(H(Msg::add), b, buf, sizeof(buf));
        CHECK(n2 && DecodeAdd(buf, n2, &h, &o) && !o.has_pcc);
        // truncated / extended
        CHECK(!DecodeAdd(buf, n2 - 1, &h, &o));
        b.hid = 0;
        CHECK(EncodeAdd(H(Msg::add), b, buf, sizeof(buf)) == 0);
    }
    {
        GBuild b;
        b.req.nonce = 7, b.req.n = 1;
        b.has_pcc = true;
        b.pcc = PccXf();
        b.body[0] = 96, b.body[1] = -200, b.body[2] = 30;
        b.rec = Rec(15);
        const std::size_t n = EncodeGBuild(H(Msg::gbuild, kFlagDev | kFlagRetry), b, buf, sizeof(buf));
        CHECK(n > 0 && n <= kMaxMessage);
        Header h;
        GBuild o;
        CHECK(DecodeGBuild(buf, n, &h, &o) && o.req == b.req && o.rec.Type() == 15 && o.has_pcc && h.flags == (kFlagDev | kFlagRetry));
        b.req = ReqId{};
        CHECK(EncodeGBuild(H(Msg::gbuild), b, buf, sizeof(buf)) == 0);
    }
    {
        Rej b;
        b.req.nonce = 7, b.req.n = 2;
        b.reason = RejReason::too_far;
        const std::size_t n = EncodeRej(H(Msg::rej), b, buf, sizeof(buf));
        Header h;
        Rej o;
        CHECK(n && DecodeRej(buf, n, &h, &o) && o.reason == RejReason::too_far && o.req == b.req);
    }
    {
        Rem b;
        b.hid = 10002;
        b.type = 9;
        b.pos[0] = 1, b.pos[1] = 2, b.pos[2] = 3;
        b.reason = RemReason::manual;
        for (Msg t : {Msg::rem, Msg::remreq}) {
            const std::size_t n = EncodeRem(H(t, kFlagAnim), b, buf, sizeof(buf));
            Header h;
            Rem o;
            CHECK(n && DecodeRem(buf, n, &h, &o) && h.type == static_cast<std::uint8_t>(t) && o.hid == 10002 && o.type == 9 && o.reason == RemReason::manual);
        }
        CHECK(EncodeRem(H(Msg::add), b, buf, sizeof(buf)) == 0);
    }
    {
        Check b;
        b.count = 300, b.hash = 0x123456789ull;
        const std::size_t n = EncodeCheck(H(Msg::check), b, buf, sizeof(buf));
        Header h;
        Check o;
        CHECK(n && DecodeCheck(buf, n, &h, &o) && o.count == 300 && o.hash == b.hash);
        Lost l;
        l.hid = 10003;
        const std::size_t m = EncodeLost(H(Msg::lost), l, buf, sizeof(buf));
        Lost lo;
        CHECK(m && DecodeLost(buf, m, &h, &lo) && lo.hid == 10003);
    }
}

void TestHeaderRejections() {
    std::uint8_t buf[kMaxMessage];
    Lost l;
    l.hid = 5;
    const std::size_t n = EncodeLost(H(Msg::lost), l, buf, sizeof(buf));
    Header h;
    Lost o;
    CHECK(DecodeLost(buf, n, &h, &o));
    std::uint8_t c[kMaxMessage];
    std::memcpy(c, buf, n);
    c[0] = 'X';
    CHECK(!DecodeLost(c, n, &h, &o));  // magic
    std::memcpy(c, buf, n);
    c[5] = 2;
    CHECK(!DecodeLost(c, n, &h, &o));  // version
    std::memcpy(c, buf, n);
    c[6] = 0x80;
    CHECK(!DecodeLost(c, n, &h, &o));  // reserved flag bits
    std::memcpy(c, buf, n);
    std::memset(c + 12, 0, 4);
    CHECK(!DecodeLost(c, n, &h, &o));  // seq 0
    CHECK(!DecodeLost(buf, n + 1, &h, &o));  // trailing byte
    Header bad = H(Msg::lost);
    bad.type = 0;
    CHECK(WriteHeader(bad, c, sizeof(c)) == 0);
    for (int t = 1; t <= 16; ++t) CHECK(std::strcmp(MsgName(static_cast<std::uint8_t>(t)), "?") != 0);
}

void TestHashes() {
    std::vector<Entry> a = {Ent(10001, 6, 1, 2, 3), Ent(10065, 9, 4, 5, 6), Ent(10002, 15, 7, 8, 9)};
    std::vector<Entry> b = {a[2], a[0], a[1]};  // another order
    std::uint64_t ha[kBuckets], hb[kBuckets];
    BucketHashes(a, ha);
    BucketHashes(b, hb);
    CHECK(std::memcmp(ha, hb, sizeof(ha)) == 0);
    CHECK(CheckHash(ha) == CheckHash(hb));
    // state, level and flags do not count (state follows streaming; levels are W2)
    b[0].state = 1, b[0].level = 3, b[0].flags = kEntOffline;
    BucketHashes(b, hb);
    CHECK(std::memcmp(ha, hb, sizeof(ha)) == 0);
    // a moved structure changes exactly its bucket
    b[1].pos[0] += 0.5;
    BucketHashes(b, hb);
    int diff = 0;
    for (int i = 0; i < kBuckets; ++i)
        if (ha[i] != hb[i]) ++diff;
    CHECK(diff == 1);
    CHECK(ha[BucketOf(10065)] != hb[BucketOf(10065)]);
    // a sub-centimetre difference does not
    b = a;
    b[1].pos[0] += 0.001;
    BucketHashes(b, hb);
    CHECK(std::memcmp(ha, hb, sizeof(ha)) == 0);
    // a missing one changes its bucket
    b = {a[0], a[1]};
    BucketHashes(b, hb);
    CHECK(ha[BucketOf(10002)] != hb[BucketOf(10002)]);
    CHECK(ha[BucketOf(10001)] == hb[BucketOf(10001)]);
}

void TestRules() {
    // the type table: Player types of the W1 categories are synced
    for (int t : {3, 6, 9, 12, 15, 18, 31, 34, 41, 44}) CHECK(wr::IsSynced(t));
    for (int t : {0, 1, 2, 4, 5, 7, 10, 22, 25, 28, 36, 37, 38, 39, 40, 42, 45, 46, -1}) CHECK(!wr::IsSynced(t));
    CHECK(wr::IsSynced(25, true) && wr::IsSynced(28, true));
    CHECK(wr::NetOf(37) == 1 && wr::NetOf(38) == 0 && wr::NetOf(39) == 2);
    CHECK(wr::CategoryOf(31) == 13 && wr::CategoryOf(34) == 12);
    CHECK(!wr::HasPrinter(25) && !wr::HasPrinter(28) && wr::HasPrinter(6));
    // every Player type of the game's NewId switch, and only those, has net 1
    const int player[] = {3, 6, 9, 12, 15, 18, 22, 25, 28, 31, 34, 37, 41, 44};
    int n1 = 0;
    for (int t = 0; t < wr::kTypeCount; ++t)
        if (wr::NetOf(t) == 1) ++n1;
    CHECK(n1 == 14);
    for (int t : player) CHECK(wr::NetOf(t) == 1);
    // ids
    CHECK(wr::RaisedCounter(500, 10501) == 501);
    CHECK(wr::RaisedCounter(600, 10501) == 600);
    CHECK(wr::PlayerId(10001) && !wr::PlayerId(100001) && !wr::PlayerId(150));
    // the guard and the binding
    const double p[3] = {1, 2, 3}, q[3] = {1.3, 2, 3}, far3[3] = {2, 2, 3};
    CHECK(wr::GuardOk(6, p, 6, q));
    CHECK(!wr::GuardOk(6, p, 6, far3));
    CHECK(!wr::GuardOk(6, p, 9, p));
    CHECK(!wr::GuardOk(25, p, 25, q));  // ladders: 0.3 m
    CHECK(wr::BindEntry(true, 6, p, 6, q) == wr::Bind::bound);
    CHECK(wr::BindEntry(true, 9, p, 6, p) == wr::Bind::replace);
    CHECK(wr::BindEntry(false, -1, p, 6, p) == wr::Bind::mirror);
    // the guest build check
    wr::GuestBuildIn in;
    in.record_valid = true, in.host_ready = true, in.type = 6;
    in.place[0] = 10, in.body[0] = 0;
    CHECK(wr::ValidateGuestBuild(in) == wr::GuestBuild::ok);
    in.place[0] = 100;
    CHECK(wr::ValidateGuestBuild(in) == wr::GuestBuild::too_far);
    in.dev = true;
    CHECK(wr::ValidateGuestBuild(in) == wr::GuestBuild::host_real);
    in.host_allows_dev = true;
    CHECK(wr::ValidateGuestBuild(in) == wr::GuestBuild::ok);
    in.type = 7;
    CHECK(wr::ValidateGuestBuild(in) == wr::GuestBuild::type);
    in.type = 25;
    CHECK(wr::ValidateGuestBuild(in) == wr::GuestBuild::disabled);
    in.ladders = true;
    CHECK(wr::ValidateGuestBuild(in) == wr::GuestBuild::ok);
    in.host_ready = false;
    CHECK(wr::ValidateGuestBuild(in) == wr::GuestBuild::not_ready);
    in.record_valid = false;
    CHECK(wr::ValidateGuestBuild(in) == wr::GuestBuild::invalid);
    // removals
    CHECK(wr::ClassifyRemoval(true, false, 100, true) == wr::Removal::manual);
    CHECK(wr::ClassifyRemoval(false, true, 100, false) == wr::Removal::manual);
    CHECK(wr::ClassifyRemoval(false, false, 100, true) == wr::Removal::evicted);
    CHECK(wr::ClassifyRemoval(false, false, 0, false) == wr::Removal::decay);
    CHECK(wr::ClassifyRemoval(false, false, 50, false) == wr::Removal::area);
    // the online gate
    wr::GateIn g;
    CHECK(!wr::GateDrops(g));  // solo, no sync, real use: vanilla
    g.automation = true;
    CHECK(wr::GateDrops(g));   // hidden runs: everything
    g = wr::GateIn{};
    g.sync_on = true, g.role = 1;
    CHECK(!wr::GateDrops(g));  // the host's own normal structure
    g.cp_known = true, g.cp_offline = true;
    CHECK(wr::GateDrops(g));   // an offline (mod) structure
    g.cp_offline = false, g.cp_mod = true;
    CHECK(wr::GateDrops(g));
    g.cp_mod = false, g.mirroring = true;
    CHECK(wr::GateDrops(g));
    g = wr::GateIn{};
    g.sync_on = true, g.role = 2;
    CHECK(wr::GateDrops(g));   // the guest: everything
    g.sync_on = false;
    CHECK(!wr::GateDrops(g));
    // OTGC write names
    CHECK(wr::OtgcWriteName("createObject"));
    CHECK(wr::OtgcWriteName("otgc://gateway/e/deleteObject"));
    CHECK(wr::OtgcWriteName("setConstructionMaterials"));
    CHECK(!wr::OtgcWriteName("getQpidObjects"));
    CHECK(wr::OtgcWriteName("createObjectX"));  // camel case: the verb "create" then a capital
    CHECK(!wr::OtgcWriteName("creates"));
    // the game's own request names (run 20261121-150217-world-census)
    CHECK(wr::OtgcWriteName("urn:otgc_create_object"));
    CHECK(wr::OtgcWriteName("urn:otgc_set_construction_materials"));
    CHECK(wr::OtgcWriteName("urn:otgc_send_like"));
    CHECK(wr::OtgcWriteName("urn:otgc_delete_object"));
    CHECK(wr::OtgcWriteName("urn:otgc_devote_highway_resources"));
    for (const char* r : {"urn:otgc_get_ranking_scheduless", "urn:otgc_find_qpid_objects", "urn:otgc_find_missions", "urn:otgc_lookup",
                          "urn:otgc_get_highway_resources", "urn:otgc_reverse_lookup", "urn:otgc_get_player_profile", "urn:otgc_get_like_history",
                          "urn:otgc_refresh_object"})
        CHECK(!wr::OtgcWriteName(r));
    CHECK(!wr::OtgcWriteName(nullptr));
    // the dedup window
    wr::SeqWindow w;
    CHECK(w.Fresh(1) && w.Fresh(2) && !w.Fresh(1) && !w.Fresh(0));
    for (std::uint32_t s = 3; s < 300; ++s) w.Fresh(s);
    CHECK(w.Fresh(1));  // long forgotten
    // round 5: the window per sender process: a restarted partner numbers from 1 again and is not taken for repeats
    wr::SenderWindow sw;
    CHECK(sw.Sender(0x1111u));
    for (std::uint32_t s = 1; s <= 80; ++s) CHECK(sw.Fresh(s));
    CHECK(!sw.Fresh(5));                     // a real repeat of the same process
    CHECK(!sw.Sender(0x1111u) && !sw.Fresh(6));  // the same process after a new link session keeps its window
    CHECK(!sw.Sender(0));                    // unknown sender: ignored
    CHECK(sw.Sender(0x2222u));               // the restarted partner
    CHECK(sw.Fresh(1) && sw.Fresh(2) && !sw.Fresh(1) && sw.sender() == 0x2222u);
    // the gate: a world-sync guest stays dropped after world sync stopped (sync_on false)
    g = wr::GateIn{};
    g.role = 2, g.guest_locked = true, g.sync_on = false;
    CHECK(wr::GateDrops(g));
    g.role = 1;
    CHECK(!wr::GateDrops(g));                // the host falls back to vanilla after a stop
    // the pick-up needs positive circumstances
    wr::PickupIn pi;
    pi.census_has_world = true, pi.vanished_lr = 1, pi.have_body = true, pi.dist_m = 3;
    CHECK(wr::PickupPlausible(pi));
    pi.dist_m = 150;
    CHECK(!wr::PickupPlausible(pi));         // far from her body: an area clear, not her pick-up
    pi.dist_m = 3, pi.vanished_lr = 6;
    CHECK(!wr::PickupPlausible(pi));         // a mass vanish
    pi.vanished_lr = 1, pi.census_has_world = false;
    CHECK(!wr::PickupPlausible(pi));         // an empty live list
    pi.census_has_world = true, pi.have_body = false;
    CHECK(!wr::PickupPlausible(pi));
    // LOST gives up; the heal backs off
    CHECK(!wr::LostGiveUp(0) && !wr::LostGiveUp(2) && wr::LostGiveUp(3));
    CHECK(wr::HealGapMs(10000, 0) == 10000 && wr::HealGapMs(10000, 2) == 40000 && wr::HealGapMs(10000, 50) == 320000);
    // position keys
    const double a[3] = {10.1, 20.0, 3.0}, b2[3] = {10.12, 20.01, 3.0}, c[3] = {10.4, 20.0, 3.0};
    CHECK(wr::MakePosKey(6, a) == wr::MakePosKey(6, b2));
    CHECK(!(wr::MakePosKey(6, a) == wr::MakePosKey(6, c)));
    CHECK(!(wr::MakePosKey(6, a) == wr::MakePosKey(9, a)));
}

// W2: MATS / STATE / DUR round trips and rejections, and the contribution rules.
void TestW2() {
    std::uint8_t buf[kMaxMessage];
    // MATS
    Mats m;
    m.req = {0xABCD1234u, 9};
    m.hid = 10167;
    m.type = 15;
    m.pos[0] = 100, m.pos[1] = -200, m.pos[2] = 30;
    m.kind = MatsKind::build;
    const std::int32_t mm[6] = {0, 120, 240, 0, 0, 0};
    std::memcpy(m.m, mm, sizeof(mm));
    std::size_t n = EncodeMats(H(Msg::mats, kFlagDev), m, buf, sizeof(buf));
    CHECK(n == kHeaderSize + 6 + 4 + 1 + 24 + 1 + 24);
    Header h;
    Mats m2;
    CHECK(DecodeMats(buf, n, &h, &m2));
    CHECK(h.flags == kFlagDev && m2.req == m.req && m2.hid == m.hid && m2.type == 15 && m2.kind == MatsKind::build && m2.m[2] == 240 && m2.pos[1] == -200);
    CHECK(!DecodeMats(buf, n - 1, &h, &m2));
    State wrong;
    CHECK(!DecodeState(buf, n, &h, &wrong));  // a MATS is not a STATE
    {
        Mats z = m;  // nothing contributed
        std::memset(z.m, 0, sizeof(z.m));
        CHECK(EncodeMats(H(Msg::mats), z, buf, sizeof(buf)) == 0);
        Mats neg = m;
        neg.m[0] = -1;
        CHECK(EncodeMats(H(Msg::mats), neg, buf, sizeof(buf)) == 0);
        Mats big = m;
        big.m[5] = kMaxMat + 1;
        CHECK(EncodeMats(H(Msg::mats), big, buf, sizeof(buf)) == 0);
        Mats k2 = m;
        k2.kind = static_cast<MatsKind>(2);
        CHECK(EncodeMats(H(Msg::mats), k2, buf, sizeof(buf)) == 0);
        Mats far = m;
        far.pos[0] = 1e9;
        CHECK(EncodeMats(H(Msg::mats), far, buf, sizeof(buf)) == 0);
        // a tampered kind byte on the wire
        std::size_t k = EncodeMats(H(Msg::mats), m, buf, sizeof(buf));
        buf[kHeaderSize + 6 + 4 + 1 + 24] = 7;
        CHECK(!DecodeMats(buf, k, &h, &m2));
        // a negative amount on the wire
        k = EncodeMats(H(Msg::mats), m, buf, sizeof(buf));
        const std::int32_t minus = -5;
        std::memcpy(buf + k - 4, &minus, 4);
        CHECK(!DecodeMats(buf, k, &h, &m2));
    }
    // STATE
    State st;
    st.hid = 10166;
    st.type = 6;
    st.pos[0] = 1, st.pos[1] = 2, st.pos[2] = 3;
    st.level = 2;
    st.max_level = 3;
    st.levelup = LevelUp::running;
    st.why = StateWhy::applied;
    for (int i = 0; i < 6; ++i) st.own[i] = i, st.invested[i] = 10 * i, st.repair[i] = 100 + i, st.excess[i] = i == 2 ? 7 : 0;
    st.dur = 359982.0f;
    st.req = {77, 3};
    n = EncodeState(H(Msg::state), st, buf, sizeof(buf));
    CHECK(n == kHeaderSize + 4 + 1 + 24 + 4 + 72 + 4 + 6 + 24);
    State s2;
    CHECK(DecodeState(buf, n, &h, &s2));
    CHECK(s2.hid == 10166 && s2.level == 2 && s2.max_level == 3 && s2.levelup == LevelUp::running && s2.why == StateWhy::applied && s2.invested[5] == 50 &&
          s2.repair[1] == 101 && s2.excess[2] == 7 && s2.dur == 359982.0f && s2.req == st.req && s2.own[4] == 4);
    CHECK(!DecodeState(buf, n + 1, &h, &s2));
    {
        State b = st;
        b.level = 6;
        CHECK(EncodeState(H(Msg::state), b, buf, sizeof(buf)) == 0);
        b = st;
        b.levelup = static_cast<LevelUp>(3);
        CHECK(EncodeState(H(Msg::state), b, buf, sizeof(buf)) == 0);
        b = st;
        b.dur = std::numeric_limits<float>::quiet_NaN();
        CHECK(EncodeState(H(Msg::state), b, buf, sizeof(buf)) == 0);
        b = st;
        b.invested[0] = kMaxTotal + 1;
        CHECK(EncodeState(H(Msg::state), b, buf, sizeof(buf)) == 0);
        b = st;
        b.hid = 0;
        CHECK(EncodeState(H(Msg::state), b, buf, sizeof(buf)) == 0);
        b = st;  // a structure with nothing invested and no excess is fine
        std::memset(b.own, 0, sizeof(b.own));
        std::memset(b.invested, 0, sizeof(b.invested));
        std::memset(b.excess, 0, sizeof(b.excess));
        CHECK(EncodeState(H(Msg::state), b, buf, sizeof(buf)) > 0);
    }
    // DUR
    Dur d;
    for (std::uint32_t i = 0; i < kDurMax; ++i) d.e.push_back({10001 + i, 1000.0f + static_cast<float>(i)});
    n = EncodeDur(H(Msg::dur), d, buf, sizeof(buf));
    CHECK(n == kHeaderSize + 1 + kDurMax * 8 && n <= kMaxMessage);
    Dur d2;
    CHECK(DecodeDur(buf, n, &h, &d2) && d2.e.size() == kDurMax && d2.e[99].hid == 10100 && d2.e[99].dur == 1099.0f);
    CHECK(!DecodeDur(buf, n - 1, &h, &d2));
    d.e.push_back({1, 1});
    CHECK(EncodeDur(H(Msg::dur), d, buf, sizeof(buf)) == 0);  // too many
    CHECK(EncodeDur(H(Msg::dur), Dur{}, buf, sizeof(buf)) == 0);
    Dur bad;
    bad.e.push_back({0, 5});
    CHECK(EncodeDur(H(Msg::dur), bad, buf, sizeof(buf)) == 0);
    bad.e[0] = {5, std::numeric_limits<float>::infinity()};
    CHECK(EncodeDur(H(Msg::dur), bad, buf, sizeof(buf)) == 0);
    // an ADD with a full record and printer place still fits beside the W2 messages
    CHECK(std::string(MsgName(12)) == "MATS" && std::string(MsgName(13)) == "STATE" && std::string(MsgName(14)) == "DUR");

    // ---- rules
    // the contributable set: the synced Player structures and the Stage ones of the same categories; not Net, roads,
    // ladders, ropes, ziplines, cargo pods
    CHECK(wr::Contributable(6) && wr::Contributable(5) && !wr::Contributable(7));    // postbox player / stage / net
    CHECK(wr::Contributable(31) && wr::Contributable(30) && !wr::Contributable(32)); // bridge 30 m
    CHECK(wr::Contributable(3) && wr::Contributable(2));                             // safe house player / stage
    CHECK(!wr::Contributable(36));                                                   // the auto-paver (roads: W4)
    CHECK(!wr::Contributable(25) && !wr::Contributable(28) && !wr::Contributable(22)); // ladder, rope, zipline
    CHECK(!wr::Contributable(37) && !wr::Contributable(0) && !wr::Contributable(20) && !wr::Contributable(-1) && !wr::Contributable(46));
    // the clamp: room = need for the last level - invested
    const std::int32_t need[6] = {0, 300, 600, 0, 0, 0}, inv[6] = {0, 250, 100, 0, 0, 0};
    const std::int32_t give[6] = {5, 120, 240, 0, 0, 0};
    const wr::Clamp c = wr::ClampContribution(give, need, inv);
    CHECK(c.applied[0] == 0 && c.excess[0] == 5);
    CHECK(c.applied[1] == 50 && c.excess[1] == 70);
    CHECK(c.applied[2] == 240 && c.excess[2] == 0);
    CHECK(c.any_applied && c.any_excess);
    const std::int32_t over[6] = {0, 400, 700, 0, 0, 0};  // invested beyond the need (online players)
    const wr::Clamp c2 = wr::ClampContribution(give, need, over);
    CHECK(!c2.any_applied && c2.excess[1] == 120 && c2.excess[2] == 240);
    const std::int32_t neg[6] = {-3, 0, 0, 0, 0, 0};
    CHECK(!wr::ClampContribution(neg, need, inv).any_applied && wr::ClampContribution(neg, need, inv).excess[0] == 0);
    // the host's level-up
    CHECK(wr::HostShouldQueueLevelUp(0, false, false));
    CHECK(!wr::HostShouldQueueLevelUp(0, true, false) && !wr::HostShouldQueueLevelUp(0, false, true));
    CHECK(!wr::HostShouldQueueLevelUp(1, false, false) && !wr::HostShouldQueueLevelUp(2, false, false) && !wr::HostShouldQueueLevelUp(4, false, false));
    // the guest's follows the host's
    CHECK(wr::GuestShouldQueueLevelUp(2, 0, 1, false, false, 2));   // the host is ahead, her materials reach it
    CHECK(wr::GuestShouldQueueLevelUp(1, 2, 1, false, false, 2));   // the host's level-up runs
    CHECK(!wr::GuestShouldQueueLevelUp(1, 0, 1, false, false, 2));  // the host is not leveling: never on her own
    CHECK(!wr::GuestShouldQueueLevelUp(2, 0, 1, true, false, 2));   // hers runs
    CHECK(!wr::GuestShouldQueueLevelUp(2, 0, 1, false, true, 2));   // hers waits
    CHECK(!wr::GuestShouldQueueLevelUp(2, 1, 1, false, false, 1));  // her materials do not reach it (yet)
    CHECK(!wr::GuestShouldQueueLevelUp(2, 0, 2, false, false, 2));  // equal
    // others so that Invested is equal
    const std::int32_t hinv[6] = {10, 20, 30, 0, 0, 5}, hown[6] = {4, 0, 30, 0, 0, 0}, list[6] = {1, 25, 0, 0, 0, 0};
    std::int32_t oth[6];
    wr::OthersFor(hinv, hown, list, oth);
    CHECK(oth[0] == 5 && oth[1] == 0 && oth[2] == 0 && oth[5] == 5);
    for (int i = 0; i < 6; ++i) CHECK(hown[i] + list[i] + oth[i] >= hinv[i]);
    // durability
    CHECK(!wr::DurWorthSending(1000, 995, 3600));   // 0.5 %
    CHECK(wr::DurWorthSending(1000, 970, 3600));    // 3 %
    CHECK(wr::DurWorthSending(100, 99, 3600));      // 1 % but below 10 % of the max (and moved > 0.5 s)
    CHECK(wr::DurWorthSending(5, -1, 3600));        // crossed zero
    CHECK(!wr::DurWorthSending(360000, 359990, 360000));
    // the applied-request log
    wr::ReqLog rl;
    CHECK(!rl.Seen(1, 1));
    rl.Add(1, 1);
    CHECK(rl.Seen(1, 1) && !rl.Seen(1, 2) && !rl.Seen(2, 1));
    for (std::uint16_t k = 2; k < 600; ++k) rl.Add(1, k);
    CHECK(!rl.Seen(1, 1) && rl.Seen(1, 599));  // long forgotten, the newest kept
}

// W3: ladders and climbing ropes
void TestW3() {
    // which types
    for (int t : {24, 25, 26, 27, 28, 29}) CHECK(wr::LadderOrRope(t));
    for (int t : {6, 22, 31, 36, 37, -1, 46}) CHECK(!wr::LadderOrRope(t));
    CHECK(wr::IsSynced(25, true) && wr::IsSynced(28, true) && !wr::IsSynced(24, true) && !wr::IsSynced(26, true) && !wr::IsSynced(29, true));
    CHECK(!wr::Contributable(25) && !wr::Contributable(28));  // no levels to pay into
    const double tol_l = wr::GuardTol(25), tol_r = wr::GuardTol(28), tol_p = wr::GuardTol(6);
    CHECK(tol_l == 0.3 && tol_r == 0.3 && tol_p == 0.5);
    // a ladder's hash ignores its position (re-attached to the ground in each world); a postbox's does not
    Entry la, lb;
    la.hid = lb.hid = 10166, la.type = lb.type = 25;
    la.pos[0] = 1.00, lb.pos[0] = 1.013;
    CHECK(EntryHash(la) == EntryHash(lb));
    la.type = lb.type = 6;
    CHECK(EntryHash(la) != EntryHash(lb));
    // removals: a ladder / rope gone with durability left is a pick-up; manual, eviction and decay first
    CHECK(wr::ClassifyRemoval(false, false, 100, false, true) == wr::Removal::pickup);
    CHECK(wr::ClassifyRemoval(false, false, 0, false, true) == wr::Removal::decay);
    CHECK(wr::ClassifyRemoval(false, false, 100, true, true) == wr::Removal::evicted);
    CHECK(wr::ClassifyRemoval(true, false, 100, false, true) == wr::Removal::manual);
    CHECK(wr::ClassifyRemoval(false, false, 100, false, false) == wr::Removal::area);
    const wr::Removal pk = wr::ClassifyRemoval(false, false, 1, false, true);
    CHECK(static_cast<int>(pk) == static_cast<int>(RemReason::pickup));  // the host sends the reason as the REM's
    // the guest: her ladder vanished with durability left = her pick-up; a decayed one or a PCC structure = LOST
    CHECK(wr::GuestVanished(25, 500) == wr::Vanished::pickup);
    CHECK(wr::GuestVanished(28, 1) == wr::Vanished::pickup);
    CHECK(wr::GuestVanished(25, 0) == wr::Vanished::lost);
    CHECK(wr::GuestVanished(6, 500) == wr::Vanished::lost);
    // the subclass fields
    Record r = Rec();
    r.b[0] = 25;
    float len = 9.65f;
    std::memcpy(r.b + RecordOffset(0x57C), &len, 4);
    std::memcpy(r.b + RecordOffset(0x580), &len, 4);
    CHECK(SubclassValid(r) && RecordValid(r));
    float bad = std::numeric_limits<float>::quiet_NaN();
    Record x = r;
    std::memcpy(x.b + RecordOffset(0x57C), &bad, 4);
    CHECK(!SubclassValid(x) && !RecordValid(x));
    bad = 500.0f;
    x = r;
    std::memcpy(x.b + RecordOffset(0x580), &bad, 4);
    CHECK(!RecordValid(x));
    x = r;
    const std::int32_t flag = 7;  // the ladder flags: any value (== 1 is read)
    std::memcpy(x.b + RecordOffset(0x584), &flag, 4);
    CHECK(RecordValid(x));
    // a rope: length, direction, longest
    r.b[0] = 28;
    const float rope[5] = {12.0f, 0.0f, 1.0f, 0.0f, 30.0f};
    std::memcpy(r.b + RecordOffset(0x578), rope, sizeof(rope));
    CHECK(RecordValid(r));
    x = r;
    bad = std::numeric_limits<float>::infinity();
    std::memcpy(x.b + RecordOffset(0x584), &bad, 4);
    CHECK(!RecordValid(x));
    x = r;
    bad = 400.0f;
    std::memcpy(x.b + RecordOffset(0x578), &bad, 4);
    CHECK(!RecordValid(x));
    // the fields cross in the Record (ADD / GBUILD) and come back in the info
    std::uint8_t info[kInfoSize] = {};
    RecordToInfo(r, info);
    float back[5];
    std::memcpy(back, info + 0x578, sizeof(back));
    CHECK(std::memcmp(back, rope, sizeof(rope)) == 0);
    std::uint8_t buf[kMaxMessage];
    Header h;
    h.type = static_cast<std::uint8_t>(Msg::gbuild);
    h.seq = 9;
    GBuild g;
    g.req = {7, 1};
    g.rec = r;
    g.body[0] = 100;
    const std::size_t n = EncodeGBuild(h, g, buf, sizeof(buf));
    CHECK(n > 0);
    Header hh;
    GBuild gb;
    CHECK(DecodeGBuild(buf, n, &hh, &gb) && gb.rec == r);
    g.rec = x;  // a rope length out of range: not sent
    CHECK(EncodeGBuild(h, g, buf, sizeof(buf)) == 0);
    // HELLO parts bit2 (ladders)
    Hello he;
    he.on = 1;
    he.parts = 7;
    h.type = static_cast<std::uint8_t>(Msg::hello);
    const std::size_t hn = EncodeHello(h, he, buf, sizeof(buf));
    Hello hb;
    CHECK(hn > 0 && DecodeHello(buf, hn, &hh, &hb) && hb.parts == 7);
    // REMREQ pickup
    Rem rm;
    rm.hid = 10170;
    rm.type = 25;
    rm.reason = RemReason::pickup;
    h.type = static_cast<std::uint8_t>(Msg::remreq);
    const std::size_t rn = EncodeRem(h, rm, buf, sizeof(buf));
    Rem rb;
    CHECK(rn > 0 && DecodeRem(buf, rn, &hh, &rb) && rb.reason == RemReason::pickup && rb.type == 25);
    CHECK(std::strcmp(RemName(5), "pickup") == 0);
}

// W4: ziplines (category 9) and the shared durability of ladders / ropes
void TestW4() {
    for (int t : {21, 22, 23}) CHECK(wr::Zipline(t));
    for (int t : {6, 25, 28, 36, -1, 46}) CHECK(!wr::Zipline(t));
    // only the Player zipline, only with the part on
    CHECK(!wr::IsSynced(22) && !wr::IsSynced(22, true) && wr::IsSynced(22, false, true) && wr::IsSynced(22, true, true));
    CHECK(!wr::IsSynced(21, true, true) && !wr::IsSynced(23, true, true));
    CHECK(wr::SyncedCategory(9, false, true) && !wr::SyncedCategory(9, true, false));
    // the other parts are unchanged by the zipline flag
    CHECK(wr::IsSynced(6, false, true) && !wr::IsSynced(25, false, true) && wr::IsSynced(25, true, true));
    // contributions: the Player zipline and the save's Stage ziplines (same ids) with the part shared
    CHECK(!wr::Contributable(22) && wr::Contributable(22, true) && wr::Contributable(21, true) && !wr::Contributable(23, true));
    CHECK(wr::Contributable(6, true) && !wr::Contributable(25, true));
    CHECK(wr::HasPrinter(22) && wr::GuardTol(22) == 0.5);
    // durability: contributable ones, and the shared Player ladders / ropes (not the Net / Stage ones, not unshared)
    CHECK(wr::DurShared(6, false, false) && wr::DurShared(22, false, true) && !wr::DurShared(22, false, false));
    CHECK(wr::DurShared(25, true, false) && wr::DurShared(28, true, false) && !wr::DurShared(25, false, false));
    CHECK(!wr::DurShared(26, true, true) && !wr::DurShared(24, true, true) && !wr::DurShared(37, true, true) && !wr::DurShared(36, true, true));
    // the zipline's subclass byte (+0x578 = cp+0x520 as a u32)
    Record r = Rec();
    r.b[0] = 22;
    std::uint32_t v = 2;
    std::memcpy(r.b + RecordOffset(0x578), &v, 4);
    CHECK(SubclassValid(r) && RecordValid(r));
    v = 0x100;
    Record x = r;
    std::memcpy(x.b + RecordOffset(0x578), &v, 4);
    CHECK(!SubclassValid(x) && !RecordValid(x));
    std::uint8_t info[kInfoSize] = {};
    RecordToInfo(r, info);
    CHECK(info[0x578] == 2);
    // the hash keeps a zipline's position (it is not re-attached)
    Entry za, zb;
    za.hid = zb.hid = 10170, za.type = zb.type = 22;
    za.pos[0] = 1.00, zb.pos[0] = 1.02;
    CHECK(EntryHash(za) != EntryHash(zb));
    // HELLO parts bit3 (ziplines); bits above stay refused
    std::uint8_t buf[kMaxMessage];
    Header h;
    h.type = static_cast<std::uint8_t>(Msg::hello);
    h.seq = 3;
    Hello he;
    he.on = 1;
    he.parts = 15;
    const std::size_t hn = EncodeHello(h, he, buf, sizeof(buf));
    Header hh;
    Hello hb;
    CHECK(hn > 0 && DecodeHello(buf, hn, &hh, &hb) && hb.parts == 15);
    he.parts = 16;
    CHECK(EncodeHello(h, he, buf, sizeof(buf)) == 0);
    he.parts = 15;
    const std::size_t hn2 = EncodeHello(h, he, buf, sizeof(buf));
    buf[kHeaderSize + 3] = 0x1F;  // parts with bit4: refused
    CHECK(hn2 > 0 && !DecodeHello(buf, hn2, &hh, &hb));
    // a zipline GBUILD crosses
    h.type = static_cast<std::uint8_t>(Msg::gbuild);
    GBuild g;
    g.req = {9, 2};
    g.rec = r;
    const std::size_t n = EncodeGBuild(h, g, buf, sizeof(buf));
    GBuild gb;
    CHECK(n > 0 && DecodeGBuild(buf, n, &hh, &gb) && gb.rec == r && gb.rec.Type() == 22);
    g.rec = x;
    CHECK(EncodeGBuild(h, g, buf, sizeof(buf)) == 0);
    // a guest zipline build: valid with the part (ladders flag irrelevant), "disabled" without it
    wr::GuestBuildIn in;
    in.record_valid = true;
    in.host_ready = true;
    in.type = 22;
    in.dev = true;
    in.host_allows_dev = true;
    CHECK(wr::ValidateGuestBuild(in) == wr::GuestBuild::disabled);
    in.ziplines = true;
    CHECK(wr::ValidateGuestBuild(in) == wr::GuestBuild::ok);
}

}  // namespace

int main() {
    TestRecord();
    TestRoundTrips();
    TestHeaderRejections();
    TestHashes();
    TestRules();
    TestW2();
    TestW3();
    TestW4();
    if (g_fail) {
        std::printf("st_world_wire_tests: %d of %d checks FAILED\n", g_fail, g_checks);
        return 1;
    }
    std::printf("st_world_wire_tests: all %d checks passed\n", g_checks);
    return 0;
}
