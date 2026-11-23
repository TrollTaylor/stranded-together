// Fragile's edited textures at runtime (round 6 T2, docs/textures.md), the parts without the game: the blob the mod builds
// from the user's pictures and the strict validator the receiver runs before the game sees a byte (tex_blob), the STT1
// messages (texture_wire), the receiver's reassembly and the sender's pacing. Generated pixels only, no game data.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "tex_blob.hpp"
#include "tex_codec.hpp"
#include "tex_hash.hpp"
#include "tex_slots.hpp"
#include "texture_wire.hpp"

using namespace st::tex;
namespace tw = st::twire;

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

Image Checker(std::uint32_t w, std::uint32_t h) {
    Image img;
    img.Resize(w, h);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            std::uint8_t* p = img.At(x, y);
            const bool c = ((x / 64) ^ (y / 64)) & 1;
            p[0] = c ? 230 : 20;
            p[1] = c ? 20 : 220;
            p[2] = static_cast<std::uint8_t>((x * 255) / w);
            p[3] = static_cast<std::uint8_t>((y * 255) / h);
        }
    return img;
}

void TestGuidBytes() {
    std::uint8_t g[16];
    CHECK(GuidBytes("2d99cdfc-7b94-c338-90e2-9d1c9ba236b6", g));
    const std::uint8_t want[16] = {0xfc, 0xcd, 0x99, 0x2d, 0x94, 0x7b, 0x38, 0xc3, 0x90, 0xe2, 0x9d, 0x1c, 0x9b, 0xa2, 0x36, 0xb6};
    CHECK(std::memcmp(g, want, 16) == 0);
    CHECK(!GuidBytes("2d99cdfc-7b94", g));
    CHECK(!GuidBytes("zz99cdfc-7b94-c338-90e2-9d1c9ba236b6", g));
    for (const Slot& s : kSlots) CHECK(GuidBytes(s.content_guid, g));
}

void TestBlobSizes() {
    CHECK(SlotIndex("S0") == 0 && SlotIndex("B2") == 6 && SlotIndex("X9") == -1);
    CHECK(BlobSize(kSlots[SlotIndex("S0")]) == kBlobHeader + 5592432);  // 2048 BC3 full chain
    CHECK(BlobSize(kSlots[SlotIndex("S2")]) == kBlobHeader + 5592432);  // 2048 BC5
    CHECK(BlobSize(kSlots[SlotIndex("S1")]) == kBlobHeader + 1398128);  // 1024 BC3
    CHECK(BlobSize(kSlots[SlotIndex("S3")]) == kBlobHeader + 699064);   // 1024 BC1
    for (const Slot& s : kSlots) CHECK(BlobSize(s) <= kBlobMax);
    CHECK(SlotMipKind(kSlots[0]) == MipKind::srgb_rgb);
    CHECK(SlotMipKind(kSlots[2]) == MipKind::normal_rg);
    CHECK(SlotMipKind(kSlots[1]) == MipKind::linear);
}

void TestBlobBuildValidate() {
    const Slot& s = kSlots[SlotIndex("S1")];  // 1024 BC3, linear, views 0x6DB
    std::vector<std::uint8_t> blob;
    std::string err;
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(BuildBlob(s, Checker(512, 512), blob, &err));  // another size: resized to the slot's
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("BuildBlob S1 (1024 BC3, resized from 512, 11 mips): %.0f ms\n", ms);
    CHECK(blob.size() == BlobSize(s));
    // exact header
    CHECK(blob[0] == 0 && blob[2] == 0x00 && blob[3] == 0x04 && blob[4] == 0x00 && blob[5] == 0x04);
    CHECK(blob[8] == 11 && blob[9] == kBC3 && blob[0x0B] == 0 && blob[0x0D] == 0xA9 && blob[0x0E] == 0);
    CHECK(std::memcmp(blob.data() + 0x10, "STTX", 4) == 0);
    std::uint32_t data = 0, emb = 0;
    std::memcpy(&data, blob.data() + 0x20, 4);
    std::memcpy(&emb, blob.data() + 0x24, 4);
    CHECK(emb == 1398128 && data == 20 + emb);
    std::uint64_t views = 0;
    std::memcpy(&views, blob.data() + 0x30, 8);
    CHECK(views == 0x6DB);
    Sha256 id{};
    CHECK(HashSha256(blob.data(), blob.size(), id));
    CHECK(ValidateBlob(s, blob.data(), blob.size(), &id, &err));
    CHECK(ValidateBlob(s, blob.data(), blob.size(), nullptr, &err));
    // deterministic
    std::vector<std::uint8_t> again;
    CHECK(BuildBlob(s, Checker(512, 512), again, &err));
    CHECK(again == blob);
    // the first mip decodes back close to the (resized) input
    Image dec;
    CHECK(Decode(kBC3, blob.data() + kBlobHeader, static_cast<std::size_t>(MipBytes(kBC3, 1024, 1024)), 1024, 1024, dec, &err));
    const Image ref = Resize(Checker(512, 512), 1024, 1024, MipKind::linear);
    CHECK(Psnr(dec, ref, 0x7) > 28.0);
    // every header byte mutated is refused (the GUID bytes too)
    int refused = 0;
    for (std::size_t i = 0; i < kBlobHeader; ++i) {
        std::vector<std::uint8_t> m = blob;
        m[i] ^= 0x5A;
        if (!ValidateBlob(s, m.data(), m.size(), nullptr, &err)) ++refused;
    }
    CHECK(refused == static_cast<int>(kBlobHeader));
    // a pixel byte changed: the GUID no longer matches; with the id: the hash
    {
        std::vector<std::uint8_t> m = blob;
        m[kBlobHeader + 1000] ^= 1;
        CHECK(!ValidateBlob(s, m.data(), m.size(), nullptr, &err));
        CHECK(!ValidateBlob(s, m.data(), m.size(), &id, &err));
    }
    // truncated, extended, another slot
    CHECK(!ValidateBlob(s, blob.data(), blob.size() - 1, nullptr, &err));
    std::vector<std::uint8_t> longer = blob;
    longer.push_back(0);
    CHECK(!ValidateBlob(s, longer.data(), longer.size(), nullptr, &err));
    CHECK(!ValidateBlob(kSlots[SlotIndex("S3")], blob.data(), blob.size(), nullptr, &err));
    CHECK(!ValidateBlob(s, nullptr, 0, nullptr, &err));
    // random garbage of the right size never passes
    std::uint32_t rng = 99;
    int passed = 0;
    for (int k = 0; k < 200; ++k) {
        std::vector<std::uint8_t> m = blob;
        for (int j = 0; j < 8; ++j) {
            rng = rng * 1664525u + 1013904223u;
            m[(rng >> 8) % 0x38] = static_cast<std::uint8_t>(rng >> 24);
        }
        if (m != blob && ValidateBlob(s, m.data(), m.size(), nullptr, &err)) ++passed;
    }
    CHECK(passed == 0);
}

void TestBlobSrgbNormal() {
    std::vector<std::uint8_t> blob;
    std::string err;
    const Slot& s0 = kSlots[SlotIndex("S0")];
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(BuildBlob(s0, Checker(2048, 2048), blob, &err));
    std::printf("BuildBlob S0 (2048 BC3 sRGB, 12 mips): %.0f ms\n",
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    CHECK(blob[0x0B] == 1 && blob[8] == 12);
    CHECK(ValidateBlob(s0, blob.data(), blob.size(), nullptr, &err));
    const Slot& s2 = kSlots[SlotIndex("S2")];
    Image n;
    n.Resize(256, 256);
    for (std::size_t i = 0; i < n.rgba.size(); i += 4) n.rgba[i] = 128, n.rgba[i + 1] = 128, n.rgba[i + 2] = 255, n.rgba[i + 3] = 255;
    CHECK(BuildBlob(s2, n, blob, &err));
    CHECK(blob[9] == kBC5U && ValidateBlob(s2, blob.data(), blob.size(), nullptr, &err));
    const Slot& s3 = kSlots[SlotIndex("S3")];
    CHECK(BuildBlob(s3, Checker(1024, 1024), blob, &err));
    CHECK(blob[9] == kBC1 && ValidateBlob(s3, blob.data(), blob.size(), nullptr, &err));
}

void TestWireMessages() {
    std::uint8_t buf[tw::kMaxMessage];
    tw::Look l;
    l.epoch = 7;
    l.table = kTableVersion;
    l.flags = 2;
    l.colour = 16;
    l.n = 2;
    l.e[0].slot = 0;
    l.e[0].size = 5592488;
    std::memset(l.e[0].id, 0xAB, 32);
    l.e[1].slot = 1;
    l.e[1].size = 1398184;
    std::memset(l.e[1].id, 0xCD, 32);
    const std::size_t n = tw::EncodeLook(l, buf, sizeof(buf));
    CHECK(n == 6 + 4 + 4 + 5 + 2 * 37);
    CHECK(tw::PeekKind(buf, n) == tw::Kind::look);
    tw::Look d;
    CHECK(tw::DecodeLook(buf, n, d));
    CHECK(d.epoch == 7 && d.table == kTableVersion && d.colour == 16 && d.n == 2 && d.e[1].size == 1398184 && d.e[1].id[5] == 0xCD);
    CHECK(!tw::DecodeLook(buf, n - 1, d));      // short
    std::vector<std::uint8_t> extra(buf, buf + n);
    extra.push_back(0);
    CHECK(!tw::DecodeLook(extra.data(), extra.size(), d));  // trailing byte
    {
        std::vector<std::uint8_t> m(buf, buf + n);
        m[4] = 2;  // version
        CHECK(tw::PeekKind(m.data(), m.size()) == tw::Kind::none);
        m = std::vector<std::uint8_t>(buf, buf + n);
        m[6 + 4 + 4 + 4] = 9;  // n > 8
        CHECK(!tw::DecodeLook(m.data(), m.size(), d));
        m = std::vector<std::uint8_t>(buf, buf + n);
        m[6 + 4 + 4] = 0x80;  // unknown flag
        CHECK(!tw::DecodeLook(m.data(), m.size(), d));
    }
    tw::Look dup = l;
    dup.e[1].slot = 0;  // one blob per slot
    const std::size_t nd = tw::EncodeLook(dup, buf, sizeof(buf));
    CHECK(nd > 0 && !tw::DecodeLook(buf, nd, d));
    tw::Look big = l;
    big.e[0].size = tw::kMaxBlob + 1;
    CHECK(tw::EncodeLook(big, buf, sizeof(buf)) == 0);
    // HAVE / WANT
    tw::IdList w;
    w.epoch = 3;
    w.n = 2;
    std::memset(w.id[0], 1, 32);
    std::memset(w.id[1], 2, 32);
    w.offset[1] = 980 * 5;
    const std::size_t nw = tw::EncodeIds(tw::Kind::want, w, buf, sizeof(buf));
    tw::IdList wd;
    CHECK(nw > 0 && tw::DecodeIds(buf, nw, tw::Kind::want, wd) && wd.n == 2 && wd.offset[1] == 4900 && wd.id[1][0] == 2);
    CHECK(!tw::DecodeIds(buf, nw, tw::Kind::have, wd));
    const std::size_t nh = tw::EncodeIds(tw::Kind::have, w, buf, sizeof(buf));
    CHECK(nh == nw - 8 && tw::DecodeIds(buf, nh, tw::Kind::have, wd) && wd.n == 2);
    // CHUNK
    std::uint8_t data[980];
    for (int i = 0; i < 980; ++i) data[i] = static_cast<std::uint8_t>(i * 7);
    tw::Chunk c;
    std::memset(c.prefix, 0xAB, 8);
    c.offset = 980;
    c.len = 980;
    c.data = data;
    const std::size_t nc = tw::EncodeChunk(c, buf, sizeof(buf));
    CHECK(nc == 6 + 8 + 4 + 2 + 980 && nc <= tw::kMaxMessage);
    tw::Chunk cd;
    CHECK(tw::DecodeChunk(buf, nc, cd) && cd.offset == 980 && cd.len == 980 && std::memcmp(cd.data, data, 980) == 0);
    CHECK(!tw::DecodeChunk(buf, nc - 1, cd));
    c.len = 981;
    CHECK(tw::EncodeChunk(c, buf, sizeof(buf)) == 0);
    c.len = 0;
    CHECK(tw::EncodeChunk(c, buf, sizeof(buf)) == 0);
    // RESULT / CANCEL
    tw::Result r;
    std::memset(r.id, 9, 32);
    r.code = tw::kHashMismatch;
    const std::size_t nr = tw::EncodeResult(r, buf, sizeof(buf));
    tw::Result rd;
    CHECK(nr > 0 && tw::DecodeResult(buf, nr, rd) && rd.code == tw::kHashMismatch && rd.id[31] == 9);
    r.code = 99;
    CHECK(tw::EncodeResult(r, buf, sizeof(buf)) == 0);
    std::uint32_t ep = 0;
    const std::size_t nx = tw::EncodeCancel(42, buf, sizeof(buf));
    CHECK(nx == 10 && tw::DecodeCancel(buf, nx, ep) && ep == 42);
    CHECK(tw::PeekKind(nullptr, 0) == tw::Kind::none);
}

// Round 8: RECIPE (kind 7): the guest's ready-made blobs, made again by the host.
void TestRecipe() {
    std::uint8_t buf[tw::kMaxMessage];
    tw::Recipe r;
    r.epoch = 12;
    r.generator = 1;
    r.encoder = 1;
    std::memset(r.originals, 0x5A, 32);
    r.n = 2;
    r.e[0].slot = 0, r.e[0].colour = 16, r.e[0].variant = 0;
    std::memset(r.e[0].id, 0x11, 32);
    r.e[1].slot = 1, r.e[1].colour = 16, r.e[1].variant = 2;
    std::memset(r.e[1].id, 0x22, 32);
    const std::size_t n = tw::EncodeRecipe(r, buf, sizeof(buf));
    CHECK(n == 6 + 4 + 1 + 2 + 32 + 1 + 2 * 35);
    CHECK(tw::PeekKind(buf, n) == tw::Kind::recipe);
    tw::Recipe d;
    CHECK(tw::DecodeRecipe(buf, n, d) && d.epoch == 12 && d.generator == 1 && d.encoder == 1 && d.originals[31] == 0x5A && d.n == 2 &&
          d.e[1].slot == 1 && d.e[1].colour == 16 && d.e[1].variant == 2 && d.e[1].id[0] == 0x22);
    CHECK(!tw::DecodeRecipe(buf, n - 1, d));   // short
    {
        std::vector<std::uint8_t> m(buf, buf + n);
        m.push_back(0);
        CHECK(!tw::DecodeRecipe(m.data(), m.size(), d));   // trailing byte
        tw::Look l;
        CHECK(!tw::DecodeLook(buf, n, l));                 // not a LOOK
    }
    auto refused = [&](const tw::Recipe& bad) {
        // the encoder refuses it; a message carrying it anyway is refused by the decoder
        if (tw::EncodeRecipe(bad, buf, sizeof(buf)) != 0) return false;
        tw::Recipe ok = r;
        const std::size_t k = tw::EncodeRecipe(ok, buf, sizeof(buf));
        std::vector<std::uint8_t> m(buf, buf + k);
        m[10] = bad.generator;
        m[45] = bad.n;
        for (unsigned i = 0; i < 2 && i < bad.n; ++i) {
            m[46 + 35 * i] = bad.e[i].slot;
            m[47 + 35 * i] = bad.e[i].colour;
            m[48 + 35 * i] = bad.e[i].variant;
        }
        tw::Recipe out;
        return !tw::DecodeRecipe(m.data(), m.size(), out);
    };
    tw::Recipe b = r;
    b.generator = 0;
    CHECK(refused(b));
    b = r, b.e[0].colour = 0;
    CHECK(refused(b));
    b = r, b.e[1].colour = 17;
    CHECK(refused(b));
    b = r, b.e[1].slot = 0;   // a slot twice
    CHECK(refused(b));
    b = r, b.e[0].variant = tw::kMetalVariants;
    CHECK(refused(b));
    b = r, b.n = 0;
    CHECK(tw::EncodeRecipe(b, buf, sizeof(buf)) == 0);
    b = r, b.n = 9;
    CHECK(tw::EncodeRecipe(b, buf, sizeof(buf)) == 0);
    // eight entries fit one message
    tw::Recipe full = r;
    full.n = 8;
    for (unsigned i = 0; i < 8; ++i) full.e[i].slot = static_cast<std::uint8_t>(i), full.e[i].colour = 3, full.e[i].variant = 0;
    const std::size_t nf = tw::EncodeRecipe(full, buf, sizeof(buf));
    CHECK(nf == 326 && nf <= tw::kMaxMessage && tw::DecodeRecipe(buf, nf, d) && d.n == 8);
    // the LOOK is unchanged by the new kind; an unknown kind 8 is still none
    std::vector<std::uint8_t> k8(buf, buf + nf);
    k8[5] = 8;
    CHECK(tw::PeekKind(k8.data(), k8.size()) == tw::Kind::none);
}

void TestAssembly() {
    tw::Assembly a;
    CHECK(!a.Init(0));
    CHECK(!a.Init(tw::kMaxBlob + 1));
    const std::uint32_t size = 980 * 3 + 100;
    CHECK(a.Init(size));
    std::vector<std::uint8_t> src(size);
    for (std::uint32_t i = 0; i < size; ++i) src[i] = static_cast<std::uint8_t>(i * 31 + 5);
    CHECK(a.ResumeOffset() == 0);
    CHECK(a.Add(980, src.data() + 980, 980));
    CHECK(!a.Add(980, src.data() + 980, 980));      // duplicate
    CHECK(!a.Add(981, src.data() + 981, 980));      // misaligned
    CHECK(!a.Add(980 * 3, src.data() + 2940, 980)); // the last piece is shorter
    CHECK(!a.Add(980 * 4, src.data(), 10));         // out of range
    CHECK(a.ResumeOffset() == 0);
    CHECK(a.Add(0, src.data(), 980));
    CHECK(a.ResumeOffset() == 1960);
    CHECK(!a.Complete());
    CHECK(a.Add(1960, src.data() + 1960, 980));
    CHECK(a.Add(2940, src.data() + 2940, 100));
    CHECK(a.Complete() && a.ResumeOffset() == size && a.received() == size);
    const std::vector<std::uint8_t> got = a.Take();
    CHECK(got == src);
    CHECK(!a.Complete());
}

void TestBucket() {
    tw::Bucket b;
    b.Reset(0);
    CHECK(!b.Take(0, 100000, 4000, 980));      // empty at start
    CHECK(b.Take(20, 100000, 4000, 980));      // 2000 B after 20 ms at 100 KB/s
    CHECK(b.Take(20, 100000, 4000, 980));
    CHECK(!b.Take(20, 100000, 4000, 980));
    CHECK(b.Take(10000, 100000, 4000, 980));   // capped at the burst
    int n = 1;
    while (b.Take(10000, 100000, 4000, 980)) ++n;
    CHECK(n == 4);
    // a second of sending at 128 KB/s in 20 ms ticks moves about 128 KB
    tw::Bucket c;
    std::size_t sent = 0;
    for (int t = 0; t <= 1000; t += 20)
        while (c.Take(t, 128.0 * 1024, 4 * 980.0, 980)) sent += 980;
    CHECK(sent > 120 * 1024 && sent < 136 * 1024);
}

}  // namespace

int main() {
    TestGuidBytes();
    TestBlobSizes();
    TestBlobBuildValidate();
    TestBlobSrgbNormal();
    TestWireMessages();
    TestRecipe();
    TestAssembly();
    TestBucket();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
