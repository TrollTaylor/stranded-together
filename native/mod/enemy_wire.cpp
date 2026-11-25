// SEN1 / SEP1 codecs (enemy_wire.hpp).
#include "enemy_wire.hpp"

#include <cmath>
#include <cstring>

#include "enemy_census.hpp"

namespace st::ewire {
namespace {
constexpr double kPi = 3.14159265358979323846;
void Put16(std::uint8_t* p, std::uint16_t v) noexcept { p[0] = static_cast<std::uint8_t>(v), p[1] = static_cast<std::uint8_t>(v >> 8); }
void Put32(std::uint8_t* p, std::uint32_t v) noexcept {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
std::uint16_t Get16(const std::uint8_t* p) noexcept { return static_cast<std::uint16_t>(p[0] | p[1] << 8); }
std::uint32_t Get32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 | static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}
bool EntryOk(const Enemy& e) noexcept {
    if (!enemy::IsHumanEnemyId(e.actor_id)) return false;
    if (e.flags & ~kEntryFlagsKnown) return false;  // round 23 B: bits 0-3 only (kInVehicle the newest)
    for (int k = 0; k < 3; ++k) {
        if (!std::isfinite(e.pos[k]) || std::fabs(e.pos[k]) >= kMaxCoordM) return false;
        if (!std::isfinite(e.vel[k])) return false;
    }
    return std::isfinite(e.yaw);
}
std::int16_t VelCm(float v) noexcept {
    const double c = std::round(static_cast<double>(v) * 100.0);
    return static_cast<std::int16_t>(c > 32700 ? 32700 : c < -32700 ? -32700 : c);
}
}  // namespace

std::uint16_t YawToU16(float yaw) noexcept {
    double turn = static_cast<double>(yaw) / (2.0 * kPi);
    turn -= std::floor(turn);
    return static_cast<std::uint16_t>(static_cast<std::uint32_t>(std::llround(turn * 65536.0)) & 0xFFFFu);
}
float YawFromU16(std::uint16_t u) noexcept {
    double a = u / 65536.0 * 2.0 * kPi;
    if (a >= kPi) a -= 2.0 * kPi;
    return static_cast<float>(a);
}

std::size_t EncodeEnemies(const EnemyFrame& f, std::uint8_t* out, std::size_t cap) noexcept {
    if (!out || f.n == 0 || f.n > kSenMax) return 0;
    const std::size_t size = kSenHeader + kSenEntry * f.n;
    if (cap < size) return 0;
    for (unsigned i = 0; i < f.n; ++i)
        if (!EntryOk(f.e[i])) return 0;
    std::memset(out, 0, size);
    out[0] = 'S', out[1] = 'E', out[2] = 'N', out[3] = '1';
    out[4] = kVersion;
    out[5] = f.n;
    Put16(out + 6, f.flags);
    Put32(out + 8, f.seq);
    Put32(out + 12, f.sender_ms);
    Put32(out + 16, f.gen_tag);
    for (unsigned i = 0; i < f.n; ++i) {
        const Enemy& e = f.e[i];
        std::uint8_t* p = out + kSenHeader + kSenEntry * i;
        Put32(p + 0, e.actor_id);
        p[4] = e.owner_epoch, p[5] = e.state, p[6] = e.phase, p[7] = e.flags;
        for (int k = 0; k < 3; ++k) Put32(p + 8 + 4 * k, static_cast<std::uint32_t>(static_cast<std::int32_t>(std::llround(e.pos[k] * 1000.0))));
        Put16(p + 20, YawToU16(e.yaw));
        for (int k = 0; k < 3; ++k) Put16(p + 22 + 2 * k, static_cast<std::uint16_t>(VelCm(e.vel[k])));
    }
    return size;
}

bool DecodeEnemies(const std::uint8_t* d, std::size_t n, EnemyFrame& out) noexcept {
    if (!d || n < kSenHeader || d[0] != 'S' || d[1] != 'E' || d[2] != 'N' || d[3] != '1' || d[4] != kVersion) return false;
    const unsigned count = d[5];
    if (count == 0 || count > kSenMax || n != kSenHeader + kSenEntry * count) return false;
    static thread_local EnemyFrame f;
    f = EnemyFrame{};
    f.n = static_cast<std::uint8_t>(count);
    f.flags = Get16(d + 6);
    f.seq = Get32(d + 8);
    f.sender_ms = Get32(d + 12);
    f.gen_tag = Get32(d + 16);
    for (unsigned i = 0; i < count; ++i) {
        const std::uint8_t* p = d + kSenHeader + kSenEntry * i;
        Enemy& e = f.e[i];
        e.actor_id = Get32(p + 0);
        e.owner_epoch = p[4], e.state = p[5], e.phase = p[6], e.flags = p[7];
        for (int k = 0; k < 3; ++k) e.pos[k] = static_cast<std::int32_t>(Get32(p + 8 + 4 * k)) / 1000.0;
        e.yaw = YawFromU16(Get16(p + 20));
        for (int k = 0; k < 3; ++k) e.vel[k] = static_cast<float>(static_cast<std::int16_t>(Get16(p + 22 + 2 * k))) / 100.0f;
        if (!EntryOk(e)) return false;
    }
    out = f;
    return true;
}

std::size_t EncodeEnemyPose(const EnemyPose& p, std::uint8_t* out, std::size_t cap) noexcept {
    if (!out || cap < kSepHeader + 1 || p.slot >= kSepSlots || !enemy::IsHumanEnemyId(p.actor_id)) return 0;
    const std::size_t j = wire::EncodeJoints(p.joints, out + kSepHeader, cap - kSepHeader);
    if (!j) return 0;
    out[0] = 'S', out[1] = 'E', out[2] = 'P', out[3] = '1';
    out[4] = kVersion;
    out[5] = p.slot;
    out[6] = out[7] = 0;
    Put32(out + 8, p.actor_id);
    Put32(out + 12, p.sender_ms);
    return kSepHeader + j;
}

// ---------------------------------------------------------------- SCB1
namespace {
constexpr std::uint8_t kScbVersion = 1;
bool AttackOk(std::uint16_t a) noexcept { return a >= 1 && a <= 0x18C && a != 0x180; }  // (0x18D: the enum's placeholder)
void PutF(std::uint8_t* p, float f) noexcept {
    std::uint32_t u = 0;
    std::memcpy(&u, &f, 4);
    Put32(p, u);
}
float GetF(const std::uint8_t* p) noexcept {
    const std::uint32_t u = Get32(p);
    float f = 0;
    std::memcpy(&f, &u, 4);
    return f;
}
std::int16_t Q15(float v) noexcept {
    double c = std::round(static_cast<double>(v) * 32767.0);
    return static_cast<std::int16_t>(c > 32767 ? 32767 : c < -32767 ? -32767 : c);
}
std::int16_t X100(float v) noexcept {
    double c = std::round(static_cast<double>(v) * 100.0);
    return static_cast<std::int16_t>(c > 32700 ? 32700 : c < -32700 ? -32700 : c);
}
std::size_t PerEntry(std::uint8_t type) noexcept {
    return type == kScbHit ? kHitSize : type == kScbHitRes ? kHitResSize : type == kScbOutcome ? kOutcomeSize : type == kScbResync ? 0
         : type == kScbOwn || type == kScbOwnAck ? kOwnSize : type == kScbOwnReset ? kOwnResetSize : SIZE_MAX;
}
unsigned MaxOf(std::uint8_t type) noexcept {
    return type == kScbHit ? kHitMax : type == kScbHitRes ? kHitResMax : type == kScbOutcome ? kOutcomeMax : type == kScbOwn || type == kScbOwnAck ? kOwnMax
         : type == kScbOwnReset ? 1 : 0;
}
bool CountOk(std::uint8_t type, unsigned n) noexcept { return type == kScbResync ? n == 0 : n >= 1 && n <= MaxOf(type); }
std::size_t Header(const ScbHeader& h, std::uint8_t type, unsigned n, std::uint8_t* out, std::size_t cap) noexcept {
    if (!out || !CountOk(type, n)) return 0;
    const std::size_t size = kScbHeader + PerEntry(type) * n;
    if (cap < size) return 0;
    std::memset(out, 0, size);
    out[0] = 'S', out[1] = 'C', out[2] = 'B', out[3] = '1';
    out[4] = type, out[5] = kScbVersion, out[6] = static_cast<std::uint8_t>(n), out[7] = 0;
    Put32(out + 8, h.guids_fp);
    Put32(out + 12, h.seq);
    return size;
}
bool HitOk(const Hit& x) noexcept {
    if (!enemy::IsHumanEnemyId(x.key) || !AttackOk(x.attack) || x.kind < kKindBullet || x.kind > kKindRestrain) return false;
    // round 23: a restrain carries no damage: k 0 / 2, a known signal, amount 0
    if (x.kind == kKindRestrain && ((x.b180 != 0 && x.b180 != 2) || x.depth < kRestrainNative || x.depth > kRestrainTest || x.amount != 0.0f)) return false;
    if (!std::isfinite(x.amount) || !std::isfinite(x.mult) || !std::isfinite(x.a20) || !std::isfinite(x.her_dist_m)) return false;
    for (int k = 0; k < 3; ++k) {
        if (!std::isfinite(x.point[k]) || std::fabs(x.point[k]) > kHitPointMaxM) return false;
        if (!std::isfinite(x.normal[k]) || !std::isfinite(x.impulse[k])) return false;
    }
    return true;
}
}  // namespace

std::size_t EncodeHits(const ScbHeader& h, const Hit* hits, unsigned n, std::uint8_t* out, std::size_t cap) noexcept {
    if (!hits) return 0;
    for (unsigned i = 0; i < n && i < kHitMax; ++i)
        if (!HitOk(hits[i])) return 0;
    const std::size_t size = Header(h, kScbHit, n, out, cap);
    if (!size) return 0;
    for (unsigned i = 0; i < n; ++i) {
        const Hit& x = hits[i];
        std::uint8_t* p = out + kScbHeader + kHitSize * i;
        Put32(p + 0, x.key);
        Put32(p + 4, x.hit_seq);
        Put16(p + 8, x.attack);
        p[10] = x.kind, p[11] = x.flags;
        Put32(p + 12, x.serial);
        p[16] = x.b12E, p[17] = x.b180, p[18] = x.depth, p[19] = 0;
        Put32(p + 20, x.v130);
        Put32(p + 24, x.v184);
        Put32(p + 28, x.v188);
        Put32(p + 32, x.part);
        PutF(p + 36, x.amount);
        PutF(p + 40, x.mult);
        PutF(p + 44, x.a20);
        for (int k = 0; k < 3; ++k) PutF(p + 48 + 4 * k, x.point[k]);
        for (int k = 0; k < 3; ++k) Put16(p + 60 + 2 * k, static_cast<std::uint16_t>(Q15(x.normal[k])));
        for (int k = 0; k < 3; ++k) Put16(p + 66 + 2 * k, static_cast<std::uint16_t>(X100(x.impulse[k])));
        Put32(p + 72, x.t_ms);
        const double dm = std::round(static_cast<double>(x.her_dist_m) * 10.0);
        Put16(p + 76, static_cast<std::uint16_t>(dm < 0 ? 0 : dm > 65535 ? 65535 : dm));
    }
    return size;
}

std::size_t EncodeHitRes(const ScbHeader& h, const HitRes* r, unsigned n, std::uint8_t* out, std::size_t cap) noexcept {
    if (!r) return 0;
    for (unsigned i = 0; i < n && i < kHitResMax; ++i)
        if (!enemy::IsHumanEnemyId(r[i].key) || r[i].result > kHitResultMax || r[i].owner > 3 || r[i].defeat > 5) return 0;
    const std::size_t size = Header(h, kScbHitRes, n, out, cap);
    if (!size) return 0;
    for (unsigned i = 0; i < n; ++i) {
        std::uint8_t* p = out + kScbHeader + kHitResSize * i;
        Put32(p + 0, r[i].key);
        Put32(p + 4, r[i].hit_seq);
        p[8] = r[i].result, p[9] = r[i].owner, p[10] = r[i].defeat, p[11] = 0;
    }
    return size;
}

std::size_t EncodeOutcomes(const ScbHeader& h, const Outcome* o, unsigned n, std::uint8_t* out, std::size_t cap) noexcept {
    if (!o) return 0;
    for (unsigned i = 0; i < n && i < kOutcomeMax; ++i)
        if (!enemy::IsHumanEnemyId(o[i].key) || o[i].defeat > 5 || o[i].cause > 3 || !std::isfinite(o[i].health) || !std::isfinite(o[i].stamina) || o[i].downbits > 0x3F)
            return 0;
    const std::size_t size = Header(h, kScbOutcome, n, out, cap);
    if (!size) return 0;
    for (unsigned i = 0; i < n; ++i) {
        std::uint8_t* p = out + kScbHeader + kOutcomeSize * i;
        Put32(p + 0, o[i].key);
        p[4] = o[i].defeat, p[5] = o[i].cause;
        Put16(p + 6, o[i].ver);
        p[8] = o[i].state, p[9] = o[i].downbits, p[10] = o[i].phase, p[11] = o[i].epoch;
        PutF(p + 12, o[i].health);
        PutF(p + 16, o[i].stamina);
        Put32(p + 20, o[i].last_seq);
    }
    return size;
}

std::size_t EncodeResync(const ScbHeader& h, std::uint8_t* out, std::size_t cap) noexcept { return Header(h, kScbResync, 0, out, cap); }

bool DecodeScbHeader(const std::uint8_t* d, std::size_t n, ScbHeader& h) noexcept {
    if (!d || n < kScbHeader || d[0] != 'S' || d[1] != 'C' || d[2] != 'B' || d[3] != '1' || d[5] != kScbVersion || d[7] != 0) return false;
    const std::uint8_t type = d[4];
    const std::size_t per = PerEntry(type);
    if (per == SIZE_MAX || !CountOk(type, d[6]) || n != kScbHeader + per * d[6]) return false;
    h.type = type, h.n = d[6];
    h.guids_fp = Get32(d + 8);
    h.seq = Get32(d + 12);
    return true;
}

bool DecodeHits(const std::uint8_t* d, std::size_t n, ScbHeader& h, Hit (&out)[kHitMax]) noexcept {
    ScbHeader hh;
    if (!DecodeScbHeader(d, n, hh) || hh.type != kScbHit) return false;
    for (unsigned i = 0; i < hh.n; ++i) {
        const std::uint8_t* p = d + kScbHeader + kHitSize * i;
        Hit x;
        x.key = Get32(p + 0);
        x.hit_seq = Get32(p + 4);
        x.attack = Get16(p + 8);
        x.kind = p[10], x.flags = p[11];
        x.serial = Get32(p + 12);
        x.b12E = p[16], x.b180 = p[17], x.depth = p[18];
        if (p[19] || Get16(p + 78)) return false;
        x.v130 = Get32(p + 20);
        x.v184 = Get32(p + 24);
        x.v188 = Get32(p + 28);
        x.part = Get32(p + 32);
        x.amount = GetF(p + 36);
        x.mult = GetF(p + 40);
        x.a20 = GetF(p + 44);
        for (int k = 0; k < 3; ++k) x.point[k] = GetF(p + 48 + 4 * k);
        for (int k = 0; k < 3; ++k) x.normal[k] = static_cast<float>(static_cast<std::int16_t>(Get16(p + 60 + 2 * k))) / 32767.0f;
        for (int k = 0; k < 3; ++k) x.impulse[k] = static_cast<float>(static_cast<std::int16_t>(Get16(p + 66 + 2 * k))) / 100.0f;
        x.t_ms = Get32(p + 72);
        x.her_dist_m = static_cast<float>(Get16(p + 76)) / 10.0f;
        if (!HitOk(x)) return false;
        out[i] = x;
    }
    h = hh;
    return true;
}

bool DecodeHitRes(const std::uint8_t* d, std::size_t n, ScbHeader& h, HitRes (&out)[kHitResMax]) noexcept {
    ScbHeader hh;
    if (!DecodeScbHeader(d, n, hh) || hh.type != kScbHitRes) return false;
    for (unsigned i = 0; i < hh.n; ++i) {
        const std::uint8_t* p = d + kScbHeader + kHitResSize * i;
        HitRes r;
        r.key = Get32(p + 0);
        r.hit_seq = Get32(p + 4);
        r.result = p[8], r.owner = p[9], r.defeat = p[10];
        if (p[11] || !enemy::IsHumanEnemyId(r.key) || r.result > kHitResultMax || r.owner > 3 || r.defeat > 5) return false;
        out[i] = r;
    }
    h = hh;
    return true;
}

bool DecodeOutcomes(const std::uint8_t* d, std::size_t n, ScbHeader& h, Outcome (&out)[kOutcomeMax]) noexcept {
    ScbHeader hh;
    if (!DecodeScbHeader(d, n, hh) || hh.type != kScbOutcome) return false;
    for (unsigned i = 0; i < hh.n; ++i) {
        const std::uint8_t* p = d + kScbHeader + kOutcomeSize * i;
        Outcome o;
        o.key = Get32(p + 0);
        o.defeat = p[4], o.cause = p[5];
        o.ver = Get16(p + 6);
        o.state = p[8], o.downbits = p[9], o.phase = p[10], o.epoch = p[11];
        o.health = GetF(p + 12);
        o.stamina = GetF(p + 16);
        o.last_seq = Get32(p + 20);
        if (!enemy::IsHumanEnemyId(o.key) || o.defeat > 5 || o.cause > 3 || o.downbits > 0x3F || !std::isfinite(o.health) || !std::isfinite(o.stamina))
            return false;
        out[i] = o;
    }
    h = hh;
    return true;
}

// ---------------------------------------------------------------- SCB1 OWN / OWNACK / OWNRESET (P1b)
namespace {
bool OwnOk(std::uint8_t type, const Own& o) noexcept {
    if (!enemy::IsHumanEnemyId(o.id) || (o.owner != 1 && o.owner != 2) || (o.phase > 5 && o.phase != 0xFF) || (o.flags & ~7u)) return false;
    if (type == kScbOwn ? (o.reason < 1 || o.reason > 14 || o.reason == 6 || o.result || o.refusal) : (o.reason != 6 || o.result > 2 || o.refusal > 9)) return false;
    for (int k = 0; k < 3; ++k)
        if (!std::isfinite(o.pos[k]) || std::fabs(o.pos[k]) >= kMaxCoordM || !std::isfinite(o.vel[k])) return false;
    return std::isfinite(o.yaw);  // health may be NaN (not carried)
}
}  // namespace

std::size_t EncodeOwns(const ScbHeader& h, std::uint8_t type, const Own* o, unsigned n, std::uint8_t* out, std::size_t cap) noexcept {
    if (!o || (type != kScbOwn && type != kScbOwnAck)) return 0;
    for (unsigned i = 0; i < n && i < kOwnMax; ++i)
        if (!OwnOk(type, o[i])) return 0;
    const std::size_t size = Header(h, type, n, out, cap);
    if (!size) return 0;
    for (unsigned i = 0; i < n; ++i) {
        const Own& x = o[i];
        std::uint8_t* p = out + kScbHeader + kOwnSize * i;
        Put32(p + 0, x.gen);
        Put32(p + 4, x.id);
        Put16(p + 8, x.epoch);
        p[10] = x.owner, p[11] = x.reason, p[12] = x.phase, p[13] = x.flags;
        Put16(p + 14, static_cast<std::uint16_t>(x.result << 8 | x.refusal));
        for (int k = 0; k < 3; ++k) Put32(p + 16 + 4 * k, static_cast<std::uint32_t>(static_cast<std::int32_t>(std::llround(x.pos[k] * 1000.0))));
        Put16(p + 28, YawToU16(x.yaw));
        for (int k = 0; k < 3; ++k) Put16(p + 30 + 2 * k, static_cast<std::uint16_t>(VelCm(x.vel[k])));
        PutF(p + 36, x.health);
    }
    return size;
}

std::size_t EncodeOwnReset(const ScbHeader& h, std::uint32_t gen, std::uint8_t* out, std::size_t cap) noexcept {
    const std::size_t size = Header(h, kScbOwnReset, 1, out, cap);
    if (size) Put32(out + kScbHeader, gen);
    return size;
}

bool DecodeOwns(const std::uint8_t* d, std::size_t n, ScbHeader& h, Own (&out)[kOwnMax]) noexcept {
    ScbHeader hh;
    if (!DecodeScbHeader(d, n, hh) || (hh.type != kScbOwn && hh.type != kScbOwnAck)) return false;
    for (unsigned i = 0; i < hh.n; ++i) {
        const std::uint8_t* p = d + kScbHeader + kOwnSize * i;
        Own x;
        x.gen = Get32(p + 0);
        x.id = Get32(p + 4);
        x.epoch = Get16(p + 8);
        x.owner = p[10], x.reason = p[11], x.phase = p[12], x.flags = p[13];
        const std::uint16_t rw = Get16(p + 14);
        x.result = static_cast<std::uint8_t>(rw >> 8), x.refusal = static_cast<std::uint8_t>(rw & 0xFF);
        for (int k = 0; k < 3; ++k) x.pos[k] = static_cast<std::int32_t>(Get32(p + 16 + 4 * k)) / 1000.0;
        x.yaw = YawFromU16(Get16(p + 28));
        for (int k = 0; k < 3; ++k) x.vel[k] = static_cast<float>(static_cast<std::int16_t>(Get16(p + 30 + 2 * k))) / 100.0f;
        x.health = GetF(p + 36);
        if (!OwnOk(hh.type, x)) return false;
        out[i] = x;
    }
    h = hh;
    return true;
}

bool DecodeOwnReset(const std::uint8_t* d, std::size_t n, ScbHeader& h, std::uint32_t* gen) noexcept {
    ScbHeader hh;
    if (!DecodeScbHeader(d, n, hh) || hh.type != kScbOwnReset) return false;
    if (gen) *gen = Get32(d + kScbHeader);
    h = hh;
    return true;
}

bool DecodeEnemyPose(const std::uint8_t* d, std::size_t n, EnemyPose& out) noexcept {
    if (!d || n <= kSepHeader || n > kSepMaxSize || d[0] != 'S' || d[1] != 'E' || d[2] != 'P' || d[3] != '1' || d[4] != kVersion) return false;
    if (d[5] >= kSepSlots || d[6] || d[7]) return false;
    const std::uint32_t id = Get32(d + 8);
    if (!enemy::IsHumanEnemyId(id)) return false;
    if (!wire::DecodeJoints(d + kSepHeader, n - kSepHeader, out.joints)) return false;
    out.slot = d[5];
    out.actor_id = id;
    out.sender_ms = Get32(d + 12);
    out.foreign_layout = out.joints.layout_hash != wire::kSamLayoutHash || out.joints.joint_count != 77;
    return true;
}

}  // namespace st::ewire
