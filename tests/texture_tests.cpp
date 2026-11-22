// Fragile's textures (docs/textures.md), the parts without the game: BCn codecs, mips, PackingInfo, the slot table, the
// manifest, PNG through WIC, the Decima archive reader on a synthetic encrypted archive, the Texture / TextureSet
// parsers, and the whole extraction (folders, seeds, re-run safety) on a synthetic game folder in %TEMP%.
// No game data is read or written.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "decima_archive.hpp"
#include "decima_core.hpp"
#include "extract_plan.hpp"
#include "png_wic.hpp"
#include "tex_codec.hpp"
#include "tex_hash.hpp"
#include "tex_manifest.hpp"
#include "tex_slots.hpp"

using namespace st::tex;

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

std::uint32_t g_rng = 12345;
std::uint32_t Rand() {
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng >> 8;
}

Image Gradient(std::uint32_t w, std::uint32_t h) {
    Image img;
    img.Resize(w, h);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            std::uint8_t* p = img.At(x, y);
            p[0] = static_cast<std::uint8_t>(x * 255 / (w - 1));
            p[1] = static_cast<std::uint8_t>(y * 255 / (h - 1));
            p[2] = static_cast<std::uint8_t>(128 + (x + y) % 64);
            p[3] = static_cast<std::uint8_t>((x * 3 + y) * 255 / (4 * (w - 1)));
        }
    return img;
}
// Smooth "noise": a few random low-frequency waves (what fabric-like detail looks like to a block coder).
Image SmoothNoise(std::uint32_t w, std::uint32_t h) {
    Image img;
    img.Resize(w, h);
    double fx[3], fy[3], ph[3];
    for (int i = 0; i < 3; ++i) fx[i] = (Rand() % 100) / 400.0, fy[i] = (Rand() % 100) / 400.0, ph[i] = (Rand() % 628) / 100.0;
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            std::uint8_t* p = img.At(x, y);
            for (int c = 0; c < 3; ++c) p[c] = static_cast<std::uint8_t>(128 + 100 * std::sin(fx[c] * x + fy[c] * y + ph[c]));
            p[3] = 255;
        }
    return img;
}
Image Checker(std::uint32_t w, std::uint32_t h) {
    Image img;
    img.Resize(w, h);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const bool on = ((x / 8) + (y / 8)) & 1;
            std::uint8_t* p = img.At(x, y);
            p[0] = on ? 230 : 20, p[1] = on ? 20 : 200, p[2] = on ? 230 : 40, p[3] = 255;
        }
    return img;
}

void Formats() {
    CHECK(MipBytes(kBC1, 1024, 1024) == 524288u);
    CHECK(MipBytes(kBC7, 2048, 2048) == 4194304u);
    CHECK(MipBytes(kBC1, 1, 1) == 8u && MipBytes(kBC3, 2, 1) == 16u);
    CHECK(ChainBytes(kBC3, 2048, 2048, 12) == 5592432u);
    CHECK(ChainBytes(kBC5U, 2048, 2048, 12) == 5592432u);
    CHECK(ChainBytes(kBC7, 1024, 1024, 11) == 1398128u);
    CHECK(ChainBytes(kBC1, 1024, 1024, 11) == 699064u);
    CHECK(ChainBytes(kBC1, 1024, 1024, 4) == 696320u);   // her BC1 mask's streamed part
    CHECK(FullMipCount(2048, 2048) == 12 && FullMipCount(1024, 1024) == 11 && FullMipCount(1, 1) == 1 && FullMipCount(256, 64) == 9);
    CHECK(CanDecode(kBC7) && !CanDecode(kBC6U) && CanEncode(kBC3) && !CanEncode(kBC7));
    CHECK(std::string(FormatName(kBC5U)) == "BC5U");
}

void Codecs() {
    const Image grad = Gradient(64, 64), noise = SmoothNoise(64, 64), check = Checker(64, 64);
    for (const Image* img : {&grad, &noise, &check}) {
        std::vector<std::uint8_t> a, b;
        CHECK(Encode(kBC1, *img, a));
        CHECK(a.size() == MipBytes(kBC1, 64, 64));
        Encode(kBC1, *img, b);
        CHECK(a == b);   // deterministic
        Image dec;
        CHECK(Decode(kBC1, a.data(), a.size(), 64, 64, dec));
        const double psnr = Psnr(*img, dec, 0x7);
        std::printf("  BC1 PSNR %.1f dB\n", psnr);
        CHECK(psnr >= 32.0);
    }
    {
        std::vector<std::uint8_t> a;
        CHECK(Encode(kBC3, grad, a) && a.size() == MipBytes(kBC3, 64, 64));
        Image dec;
        CHECK(Decode(kBC3, a.data(), a.size(), 64, 64, dec));
        const double pa = Psnr(grad, dec, 0x8), pc = Psnr(grad, dec, 0x7);
        std::printf("  BC3 alpha %.1f dB, colour %.1f dB\n", pa, pc);
        CHECK(pa >= 40.0 && pc >= 32.0);
    }
    {
        // BC4 of one channel; BC5 of a normal map: angle error.
        std::vector<std::uint8_t> a;
        EncodeBC4(grad, 0, a);
        Image dec;
        CHECK(Decode(kBC4U, a.data(), a.size(), 64, 64, dec));
        CHECK(Psnr(grad, dec, 0x1) >= 40.0);
        Image n;
        n.Resize(64, 64);
        for (std::uint32_t y = 0; y < 64; ++y)
            for (std::uint32_t x = 0; x < 64; ++x) {
                const double nx = 0.5 * std::sin(x * 0.2), ny = 0.5 * std::cos(y * 0.15);
                std::uint8_t* p = n.At(x, y);
                p[0] = static_cast<std::uint8_t>(std::lround(nx * 127.5 + 127.5));
                p[1] = static_cast<std::uint8_t>(std::lround(ny * 127.5 + 127.5));
                p[2] = 0, p[3] = 255;
            }
        RebuildNormalZ(n);
        EncodeBC5(n, a);
        CHECK(a.size() == MipBytes(kBC5U, 64, 64));
        CHECK(Decode(kBC5U, a.data(), a.size(), 64, 64, dec));
        RebuildNormalZ(dec);
        double worst = 0;
        for (std::size_t i = 0; i < n.rgba.size(); i += 4) {
            double v[2][3];
            for (int k = 0; k < 3; ++k) v[0][k] = n.rgba[i + k] / 127.5 - 1, v[1][k] = dec.rgba[i + k] / 127.5 - 1;
            const double dot = v[0][0] * v[1][0] + v[0][1] * v[1][1] + v[0][2] * v[1][2];
            const double l = std::sqrt((v[0][0] * v[0][0] + v[0][1] * v[0][1] + v[0][2] * v[0][2]) *
                                       (v[1][0] * v[1][0] + v[1][1] * v[1][1] + v[1][2] * v[1][2]));
            worst = std::max(worst, std::acos(std::min(1.0, dot / l)) * 180.0 / 3.14159265358979);
        }
        std::printf("  BC5 normal worst angle %.2f deg\n", worst);
        CHECK(worst <= 4.0);
    }
    {
        // Odd sizes: edge blocks.
        const Image odd = Gradient(13, 7);
        std::vector<std::uint8_t> a;
        CHECK(Encode(kBC1, odd, a) && a.size() == MipBytes(kBC1, 13, 7));
        Image dec;
        CHECK(Decode(kBC1, a.data(), a.size(), 13, 7, dec) && dec.w == 13 && dec.h == 7);
        CHECK(!Decode(kBC1, a.data(), a.size() - 1, 13, 7, dec));   // short data refused
        CHECK(!Decode(kBC6U, a.data(), a.size(), 4, 4, dec));
    }
    {
        // BC1's 3-colour mode (c0 <= c1): index 3 is transparent black, index 2 the midpoint.
        const std::uint8_t blk[8] = {0x00, 0x00, 0xFF, 0xFF, 0x0E, 0, 0, 0};   // c0 black, c1 white; pixel 0 idx 2, pixel 1 idx 3
        Image dec;
        CHECK(Decode(kBC1, blk, 8, 4, 4, dec));
        CHECK(dec.At(0, 0)[0] == 127 && dec.At(0, 0)[3] == 255);
        CHECK(dec.At(1, 0)[0] == 0 && dec.At(1, 0)[3] == 0);
        CHECK(dec.At(2, 0)[0] == 0 && dec.At(2, 0)[3] == 255);   // idx 0 = c0
    }
    {
        // BC7 mode 6 by hand: endpoints R0=127 G0=0 B0=0 A0=127 (p0=1) -> (255,1,1,255); e1 = (0,127,0,127) p1=0 -> (0,254,0,254).
        // Indices: pixel 0 (anchor, 3 bits) = 7, the others 4 bits = 15 (all e1).
        std::uint8_t blk[16] = {};
        int pos = 0;
        auto put = [&](std::uint32_t v, int n) {
            for (int i = 0; i < n; ++i, ++pos)
                if ((v >> i) & 1) blk[pos / 8] |= static_cast<std::uint8_t>(1u << (pos % 8));
        };
        put(0x40, 7);   // mode 6
        put(127, 7), put(0, 7);   // R0 R1
        put(0, 7), put(127, 7);   // G0 G1
        put(0, 7), put(0, 7);     // B0 B1
        put(127, 7), put(127, 7); // A0 A1
        put(1, 1), put(0, 1);     // p-bits
        put(7, 3);
        for (int i = 1; i < 16; ++i) put(15, 4);
        CHECK(pos == 128);
        Image dec;
        CHECK(Decode(kBC7, blk, 16, 4, 4, dec));
        const std::uint8_t* p0 = dec.At(0, 0);
        const std::uint8_t* p5 = dec.At(1, 1);
        // weight 30 of 64 for index 7: ((64-30)*e0 + 30*e1 + 32) >> 6
        CHECK(p0[0] == ((34 * 255 + 30 * 0 + 32) >> 6) && p0[1] == ((34 * 1 + 30 * 254 + 32) >> 6) && p0[3] == ((34 * 255 + 30 * 254 + 32) >> 6));
        CHECK(p5[0] == 0 && p5[1] == 254 && p5[2] == 0 && p5[3] == 254);
        // Mode 1 (2 subsets, partition 0 = 0xCCCC: columns 2-3 are subset 1), shared p-bits: endpoints all 0 except
        // subset 1's e0 R = 63 with its p-bit 1 -> 127<<1|1 = 255 expanded 7-bit; index 0 everywhere.
        std::memset(blk, 0, sizeof(blk));
        pos = 0;
        put(0x02, 2);       // mode 1
        put(0, 6);          // partition 0
        put(0, 6), put(0, 6), put(63, 6), put(0, 6);   // R: s0e0 s0e1 s1e0 s1e1
        for (int i = 0; i < 8; ++i) put(0, 6);          // G, B
        put(0, 1), put(1, 1);                           // shared p-bits
        Decode(kBC7, blk, 16, 4, 4, dec);
        CHECK(dec.At(0, 0)[0] == 0 && dec.At(2, 0)[0] == 255 && dec.At(3, 3)[0] == 255 && dec.At(1, 3)[0] == 0);
    }
    {
        // Timing on a full-size map (printed, not checked).
        const Image big = SmoothNoise(2048, 2048);
        std::vector<std::uint8_t> out;
        const auto t0 = std::chrono::steady_clock::now();
        EncodeBC3(big, out);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        std::printf("  2048 BC3 mip 0 encode: %lld ms\n", static_cast<long long>(ms));
    }
}

void Mips() {
    Image img;
    img.Resize(2, 2);
    for (int i = 0; i < 4; ++i) {
        std::uint8_t* p = &img.rgba[static_cast<std::size_t>(i) * 4];
        p[0] = p[1] = p[2] = (i & 1) ? 255 : 0;
        p[3] = (i & 1) ? 255 : 0;
    }
    const Image lin = Downsample(img, MipKind::linear);
    CHECK(lin.w == 1 && lin.h == 1 && lin.rgba[0] == 128 && lin.rgba[3] == 128);
    const Image srgb = Downsample(img, MipKind::srgb_rgb);
    CHECK(srgb.rgba[0] == 188 && srgb.rgba[3] == 128);   // 50% linear light in sRGB; alpha linear
    // Normals pointing +x and -x average to straight up.
    Image n;
    n.Resize(2, 1);
    n.rgba = {255, 128, 128, 255, 0, 128, 128, 255};
    const Image nd = Downsample(n, MipKind::normal_rg);
    CHECK(nd.w == 1 && nd.h == 1 && std::abs(nd.rgba[0] - 128) <= 1 && std::abs(nd.rgba[1] - 128) <= 1 && nd.rgba[2] >= 254);
    // A tilted pair stays unit length after renormalising.
    n.rgba = {200, 128, 128, 255, 180, 160, 128, 255};
    const Image nt = Downsample(n, MipKind::normal_rg);
    const double x = nt.rgba[0] / 127.5 - 1, y = nt.rgba[1] / 127.5 - 1, z = nt.rgba[2] / 127.5 - 1;
    CHECK(std::abs(std::sqrt(x * x + y * y + z * z) - 1.0) < 0.02);
    // Sizes along a chain and Resize.
    Image a = Gradient(256, 64);
    int levels = 1;
    while (a.w > 1 || a.h > 1) a = Downsample(a, MipKind::linear), ++levels;
    CHECK(levels == static_cast<int>(FullMipCount(256, 64)));
    const Image r1 = Resize(Gradient(100, 50), 2048, 2048, MipKind::srgb_rgb);
    CHECK(r1.w == 2048 && r1.h == 2048);
    const Image r2 = Resize(Gradient(2048, 2048), 1024, 1024, MipKind::linear);
    CHECK(r2.w == 1024 && r2.h == 1024);
    CHECK(SrgbToLinear8(0) == 0 && SrgbToLinear8(255) == 255 && LinearToSrgb8(0.5f) == 188);
}

void Packing() {
    // Her colour texture: RGB = Colour, A = Roughness (single-channel source).
    std::vector<PackedMap> m = MapsOf(0x86211101u);
    CHECK(m.size() == 2 && m[0].type == kTypeColor && m[0].Component() == "RGB" && m[1].type == kTypeRoughness && m[1].Component() == "A");
    CHECK(Unpack(0x86211101u, 3).single && Unpack(0x86211101u, 1).source == 1);
    m = MapsOf(0x85241404u);
    CHECK(m.size() == 2 && m[0].type == kTypeReflectance && m[1].type == kTypeAO && m[1].Component() == "A");
    m = MapsOf(0x33231303u);
    CHECK(m.size() == 1 && m[0].type == kTypeNormal && m[0].Component() == "RGBA");
    m = MapsOf(0x80281808u);   // A byte 0x80: unused (type 0)
    CHECK(m.size() == 1 && m[0].type == kTypeMask && m[0].Component() == "RGB");
    m = MapsOf(0x80848685u);   // her boots: AO R, Roughness G, Reflectance B
    CHECK(m.size() == 3 && m[0].type == kTypeAO && m[1].type == kTypeRoughness && m[2].type == kTypeReflectance && m[2].Component() == "B");
    // A packed source order: channels sorted by their source channel.
    m = MapsOf(0x00001101u | (0x21u << 16));
    CHECK(m.size() == 1 && m[0].channels == std::vector<int>({0, 1, 2}));
    // Split and repack round trip.
    const Image src = Gradient(16, 16);
    const Image colour = ExtractChannels(src, {0, 1, 2}), rough = ExtractChannels(src, {3});
    CHECK(rough.rgba[0] == src.rgba[3] && rough.rgba[1] == src.rgba[3] && rough.rgba[3] == 255);
    Image back;
    back.Resize(16, 16);
    CHECK(InsertChannels(back, colour, {0, 1, 2}) && InsertChannels(back, rough, {3}));
    CHECK(back.rgba == src.rgba);
    CHECK(std::string(MapFileName(kTypeAO)) == "Ambient occlusion" && std::string(SetTypeName(kTypeMaskAlpha)) == "Mask_Alpha");
}

void Slots() {
    // The CRC text is the rows themselves (any change of a row changes kTableVersion).
    std::string text;
    for (const Slot& s : kSlots) {
        char row[512];
        std::snprintf(row, sizeof(row), "%s|%s|%s|%s|%s|%u|%u|%u|%u|%u|%08x|%llx|%u\n", s.id, s.group == SlotGroup::suit ? "suit" : "boots",
                      s.set_guid, s.object_guid, s.content_guid, s.w, s.h, s.mips, s.orig_fmt, s.srgb, s.packing,
                      static_cast<unsigned long long>(s.views), s.clone_fmt);
        text += row;
        CHECK(s.mips == FullMipCount(s.w, s.h));
        CHECK(CanEncode(s.clone_fmt));
        // A BC7 original with its alpha used is cloned as BC3; BC1 / BC5 stay.
        const bool alpha_used = Unpack(s.packing, 3).type != 0;
        if (s.orig_fmt == kBC7) CHECK(s.clone_fmt == (alpha_used ? kBC3 : kBC1));
        else CHECK(s.clone_fmt == s.orig_fmt);
        CHECK(SlotByObjectGuid(s.object_guid) == &s);
    }
    CHECK(text == detail::kSlotRowsText);
    CHECK(kTableVersion == Crc32(detail::kSlotRowsText, std::strlen(detail::kSlotRowsText)));
    volatile std::uint32_t pinned = 0x9a182ce3u;
    CHECK(kTableVersion == pinned);   // pinned: a change here means the folder must be extracted again
    CHECK(Crc32("123456789", 9) == 0xCBF43926u);
    CHECK(SlotByObjectGuid("00000000-0000-0000-0000-000000000000") == nullptr);
}

void ManifestText() {
    Manifest m;
    m.table = kTableVersion;
    m.game = "86479872-1343";
    m.tool = "0.24.0";
    m.rows.push_back({"Fragile porter suit\\Original\\Suit - Colour.png", "S0", "RGB", 2048, 2048, std::string(64, 'a'), "original", 3827843});
    m.rows.push_back({"Fragile porter suit\\16 Gold\\Suit - Colour.png", "S0", "RGB", 2048, 2048, std::string(64, 'b'), "seed", 3827843});
    m.rows.push_back({"Sam porter suit\\16 Gold\\Suit - Reflectance.png", "ref", "RGB", 1024, 1024, std::string(64, 'c'), "ref", 5});
    const std::string text = WriteManifest(m);
    Manifest back;
    std::string err;
    CHECK(ParseManifest(text, back, err));
    CHECK(back.table == m.table && back.game == m.game && back.tool == m.tool && back.rows.size() == 3);
    CHECK(back.rows[1].kind == "seed" && back.rows[1].w == 2048 && back.rows[2].bytes == 5 && back.rows[0].slot == "S0");
    CHECK(back.Find("fragile porter suit/16 GOLD/suit - colour.png") == &back.rows[1]);
    CHECK(!ParseManifest("stx-manifest 2\n", back, err));
    CHECK(!ParseManifest("stx-manifest 1\n", back, err));   // no table line
    std::string bad = text;
    bad.replace(bad.find("original"), 8, "unknown!");
    CHECK(!ParseManifest(bad, back, err));
    bad = text;
    bad.replace(bad.find(std::string(64, 'a')), 3, "xyz");
    CHECK(!ParseManifest(bad, back, err));
    CHECK(!ParseManifest(text + "file ..\\evil.png\tref\tRGB\t1x1\t" + std::string(64, 'a') + "\tref\t1\n", back, err));
    CHECK(ParseManifest(std::string(text).insert(0, ""), back, err));
    // CRLF tolerated.
    std::string crlf;
    for (char c : text) crlf += c == '\n' ? std::string("\r\n") : std::string(1, c);
    CHECK(ParseManifest(crlf, back, err) && back.rows.size() == 3);
    // The extractor's output version (round 6): written, read back; an older manifest without it reads as 0.
    m.extract = kExtractVersion;
    CHECK(ParseManifest(WriteManifest(m), back, err) && back.extract == kExtractVersion);
    std::string old = WriteManifest(m);
    old.erase(old.find("\textract "), std::string("\textract ").size() + std::to_string(kExtractVersion).size());
    CHECK(ParseManifest(old, back, err) && back.extract == 0 && back.tool == "0.24.0");
    std::string badx = WriteManifest(m);
    badx.replace(badx.find("\textract ") + 9, 1, "x");
    CHECK(!ParseManifest(badx, back, err));
    // Folder numbers.
    CHECK(FolderNumber("16 Gold") == 16 && FolderNumber("09 my mustard") == 9 && FolderNumber("Original") == -1 && FolderNumber("9 Gold") == -1 &&
          FolderNumber("16Gold") == -1);
    CHECK(SamePath("a/B\\c", "A\\b/C") && !SamePath("a", "ab"));
}

void Names() {
    CHECK(std::string(kColourFolders[16].name) == "16 Gold" && std::string(kColourFolders[0].name) == "00 Default");
    for (int c = 0; c < 17; ++c) CHECK(kColourFolders[c].number == c && FolderNumber(kColourFolders[c].name) == c);
    for (const NamedFolder& f : kBootFolders) CHECK(FolderNumber(f.name) == f.number && f.number != 1);
    CHECK(std::string(ColourName(15)) == "Silver" && std::string(ColourName(17)).empty());
    CHECK(IsUsableSuitName("suit - colour.png") && IsUsableSuitName("Suit - Normal.png") && !IsUsableSuitName("Gloves - Colour.png"));
    CHECK(IsUsableBootName("Boots - Reflectance.png") && !IsUsableBootName("Suit - Colour.png"));
    // Every job's folder is one of the planned trees; Fragile's originals come first.
    const std::vector<ExtractJob>& jobs = ExtractJobs();
    CHECK(jobs.size() == 31);
    CHECK(jobs[0].original && std::string(jobs[0].core_path) == kSuitSet);
    int originals = 0;
    for (const ExtractJob& j : jobs) {
        originals += j.original ? 1 : 0;
        const std::string f = j.folder;
        CHECK(f.rfind(kOriginalDir, 0) == 0 || f.rfind(kSamSuitDir, 0) == 0 || f.rfind(kSamBootsDir, 0) == 0);
        CHECK(std::string(j.core_path).rfind("ds/models/characters/", 0) == 0);
    }
    CHECK(originals == 7);
    const std::string readme = ReadmeText();
    CHECK(readme.find("do not share") != std::string::npos && readme.find("16 Gold") != std::string::npos && readme.find("\r\n") != std::string::npos);
}

void Hashes() {
    std::uint64_t h[2];
    Murmur3x64_128("", 0, 42, h);
    CHECK(h[0] == 0xf02aa77dfa1b8523ull && h[1] == 0xd1016610da11cbb9ull);
    Murmur3x64_128("hello", 5, 42, h);
    CHECK(h[0] == 0xc4b8b3c960af6f08ull && h[1] == 0x2334b875b0efbc7aull);
    const char* s = "stranded together test string!!";
    Murmur3x64_128(s, std::strlen(s), 42, h);
    CHECK(h[0] == 0x7a64cb6ca2fb0a83ull && h[1] == 0xf2fdf3e49142d066ull);
    Murmur3x64_128("0123456789abcdef0123", 20, 42, h);
    CHECK(h[0] == 0x081e6d1fd710e499ull && h[1] == 0xb7f785c1dfd99b8dull);
    CHECK(CorePathHash("test/path/thing") == 0x641b99f6950ee3d3ull);
    CHECK(CorePathHash("Test\\Path\\Thing.core") == 0x641b99f6950ee3d3ull);
    CHECK(NormalizeCorePath("/A/b.core.stream") == "a/b.core.stream");
    Sha256 d{};
    CHECK(HashSha256("abc", 3, d) && Hex(d) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    Md5 m{};
    CHECK(HashMd5("abc", 3, m) && Hex(m) == "900150983cd24fb0d6963f7d28e17f72");
    // Every archive open is read-only.
    CHECK(ArchiveOpenAccess() == GENERIC_READ);
    CHECK((ArchiveShareMode() & FILE_SHARE_READ) != 0);
}

// ---- A synthetic encrypted archive -------------------------------------------------------------------------
void Put32(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) { std::memcpy(b.data() + at, &v, 4); }
void Put64(std::vector<std::uint8_t>& b, std::size_t at, std::uint64_t v) { std::memcpy(b.data() + at, &v, 8); }

// Encrypts a 32-byte table row so that the reader, which takes key0 / key1 from the encrypted row, decrypts it.
void EncryptRow(std::uint8_t* row, std::uint32_t key0, int key0_at, std::uint32_t key1) {
    std::uint8_t m0[16], m1[16];
    TableMask(key0, m0);
    TableMask(key1, m1);
    std::uint32_t k0 = key0 ^ [&] { std::uint32_t v; std::memcpy(&v, (key0_at < 16 ? m0 + key0_at : m1 + key0_at - 16), 4); return v; }();
    std::memcpy(row + key0_at, &k0, 4);
    std::uint32_t k1 = key1 ^ [&] { std::uint32_t v; std::memcpy(&v, m1 + 12, 4); return v; }();
    std::memcpy(row + 28, &k1, 4);
    DecryptTableRow(row, key0, key1);   // XOR: encrypts
}

struct FakeFile {
    std::string path;   // core path
    std::vector<std::uint8_t> bytes;
};

// Two chunks: one stored (csize == usize), one "compressed" (bytes reversed: the fake decompressor reverses them).
bool WriteArchive(const std::wstring& file, const std::vector<FakeFile>& files) {
    std::vector<std::uint8_t> stream;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> where;
    for (const FakeFile& f : files) {
        where.push_back({stream.size(), static_cast<std::uint32_t>(f.bytes.size())});
        stream.insert(stream.end(), f.bytes.begin(), f.bytes.end());
    }
    const std::uint32_t half = static_cast<std::uint32_t>(stream.size() / 2);
    const std::uint32_t sizes[2] = {half, static_cast<std::uint32_t>(stream.size() - half)};
    const std::uint32_t n_entries = static_cast<std::uint32_t>(files.size()), n_chunks = 2;
    const std::uint64_t data_at = 40 + 32ull * (n_entries + n_chunks);
    std::vector<std::uint8_t> out(static_cast<std::size_t>(data_at), 0);
    const std::uint32_t key = 0x1234567u;
    Put32(out, 0, 0x21304050u);
    Put32(out, 4, key);
    std::vector<std::uint8_t> hdr(32, 0);
    Put64(hdr, 0, 0);
    Put64(hdr, 8, stream.size());
    Put64(hdr, 16, n_entries);
    Put32(hdr, 24, n_chunks);
    Put32(hdr, 28, 1u << 20);
    DecryptTableRow(hdr.data(), key, key + 1);
    std::memcpy(out.data() + 8, hdr.data(), 32);
    for (std::uint32_t i = 0; i < n_entries; ++i) {
        std::vector<std::uint8_t> row(32, 0);
        Put32(row, 0, i);
        Put64(row, 8, CorePathHash(files[i].path));
        Put64(row, 16, where[i].first);
        Put32(row, 24, where[i].second);
        EncryptRow(row.data(), 0x1000u + i, 4, 0x2000u + i);
        std::memcpy(out.data() + 40 + 32 * i, row.data(), 32);
    }
    std::uint64_t uoff = 0;
    std::vector<std::uint8_t> data;
    for (std::uint32_t c = 0; c < n_chunks; ++c) {
        std::vector<std::uint8_t> chunk(stream.begin() + static_cast<std::ptrdiff_t>(uoff), stream.begin() + static_cast<std::ptrdiff_t>(uoff + sizes[c]));
        if (c == 1) {
            std::reverse(chunk.begin(), chunk.end());
            chunk.push_back(0xAB);   // compressed size != uncompressed size
        }
        const std::uint32_t k0 = 0x3000u + c, k1 = 0x4000u + c;
        std::vector<std::uint8_t> row(32, 0);
        Put64(row, 0, uoff);
        Put32(row, 8, sizes[c]);
        Put64(row, 16, data_at + data.size());
        Put32(row, 24, static_cast<std::uint32_t>(chunk.size()));
        std::uint8_t key16[16];
        std::memcpy(key16, row.data(), 12);
        std::memcpy(key16 + 12, &k0, 4);
        EncryptRow(row.data(), k0, 12, k1);
        std::memcpy(out.data() + 40 + 32 * (n_entries + c), row.data(), 32);
        std::uint8_t mask[16];
        ChunkMask(key16, mask);
        for (std::size_t i = 0; i < chunk.size(); ++i) chunk[i] ^= mask[i & 15];
        data.insert(data.end(), chunk.begin(), chunk.end());
        uoff += sizes[c];
    }
    out.insert(out.end(), data.begin(), data.end());
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    const bool ok = WriteFile(h, out.data(), static_cast<DWORD>(out.size()), &put, nullptr) && put == out.size();
    CloseHandle(h);
    return ok;
}

bool FakeDecompress(const std::uint8_t* src, std::size_t n, std::uint8_t* dst, std::size_t m) {
    if (n != m + 1) return false;
    for (std::size_t i = 0; i < m; ++i) dst[i] = src[m - 1 - i];
    return true;
}

// A Texture core object (embedded only: no .core.stream).
std::vector<std::uint8_t> TextureObject(std::uint8_t guid_byte, std::uint8_t fmt, std::uint32_t w, std::uint32_t h, std::uint8_t srgb,
                                        const std::vector<std::uint8_t>& mips_data, std::uint32_t mips, bool corrupt = false) {
    std::vector<std::uint8_t> body(72, 0);
    for (int i = 0; i < 16; ++i) body[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(guid_byte + i);
    std::uint8_t* hd = body.data() + 16;
    const std::uint16_t w16 = static_cast<std::uint16_t>(w), h16 = static_cast<std::uint16_t>(h);
    std::memcpy(hd + 2, &w16, 2);
    std::memcpy(hd + 4, &h16, 2);
    hd[8] = static_cast<std::uint8_t>(mips);
    hd[9] = fmt;
    hd[11] = srgb;
    hd[13] = 0xA9;
    hd[14] = 0xFF;
    for (int i = 0; i < 16; ++i) hd[16 + i] = static_cast<std::uint8_t>(0xC0 + guid_byte + i);
    body.insert(body.end(), mips_data.begin(), mips_data.end());
    Put32(body, 48, static_cast<std::uint32_t>(body.size() - 52 + (corrupt ? 1 : 0)));
    Put32(body, 52, static_cast<std::uint32_t>(mips_data.size()));
    std::vector<std::uint8_t> obj(12, 0);
    Put64(obj, 0, kTypeTexture);
    Put32(obj, 8, static_cast<std::uint32_t>(body.size()));
    obj.insert(obj.end(), body.begin(), body.end());
    return obj;
}
std::vector<std::uint8_t> SetObject(const std::vector<std::pair<int, std::uint32_t>>& entries) {   // (texture guid byte, packing)
    std::vector<std::uint8_t> body(20, 0);
    body[0] = 0x55;
    Put32(body, 16, static_cast<std::uint32_t>(entries.size()));
    for (const auto& [g, packing] : entries) {
        std::vector<std::uint8_t> e(17 + 1 + 16, 0);
        Put32(e, 9, packing);
        e[17] = 1;   // ref kind 1: a GUID
        for (int i = 0; i < 16; ++i) e[18 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(g + i);  // g: the texture GUID byte
        body.insert(body.end(), e.begin(), e.end());
    }
    body.resize(body.size() + 4 + 4 + 4 + 1, 0);   // address mode, lambda, 0 descs, empty preset
    std::vector<std::uint8_t> obj(12, 0);
    Put64(obj, 0, kTypeTextureSet);
    Put32(obj, 8, static_cast<std::uint32_t>(body.size()));
    obj.insert(obj.end(), body.begin(), body.end());
    return obj;
}
std::vector<std::uint8_t> Rgba8Chain(std::uint32_t w, std::uint32_t h, std::uint8_t seed, std::uint32_t& mips) {
    std::vector<std::uint8_t> d;
    mips = 0;
    for (;;) {
        for (std::uint32_t i = 0; i < w * h; ++i) {
            d.push_back(static_cast<std::uint8_t>(seed + i));
            d.push_back(static_cast<std::uint8_t>(seed * 3 + i * 7));
            d.push_back(static_cast<std::uint8_t>(200 - i));
            d.push_back(static_cast<std::uint8_t>(i * 13));
        }
        ++mips;
        if (w == 1 && h == 1) break;
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
    }
    return d;
}

std::wstring TempDir(const wchar_t* name) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring d = std::wstring(tmp) + L"st_texture_tests\\" + name;
    return d;
}
void RemoveTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            const std::wstring p = dir + L"\\" + n;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveTree(p);
            else {
                SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(p.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}
bool FileExists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
std::string FileHash(const std::wstring& p) {
    Sha256 d{};
    return HashFileSha256(p, d) ? Hex(d) : std::string();
}

void ArchiveReader() {
    const std::wstring dir = TempDir(L"archive");
    RemoveTree(dir);
    CreateDirectoryW(TempDir(L"").c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    std::vector<FakeFile> files;
    files.push_back({"ds/test/one", std::vector<std::uint8_t>(1000, 0x11)});
    std::vector<std::uint8_t> two(3000);
    for (std::size_t i = 0; i < two.size(); ++i) two[i] = static_cast<std::uint8_t>(i * 31);
    files.push_back({"ds/test/two.core.stream", two});
    CHECK(WriteArchive(dir + L"\\a.bin", files));
    ArchiveSet set;
    std::string err;
    CHECK(set.Open(dir, err));
    set.Index({CorePathHash("ds/test/one"), CorePathHash("ds/test/two.core.stream"), 0x1234});
    CHECK(set.Has(CorePathHash("ds/test/one")) && !set.Has(0x1234));
    std::vector<std::uint8_t> got;
    CHECK(set.ReadFile(CorePathHash("ds/test/one"), FakeDecompress, got, err) && got == files[0].bytes);
    CHECK(set.ReadFile(CorePathHash("ds/test/two.core.stream"), FakeDecompress, got, err) && got == two);   // spans both chunks
    CHECK(!set.ReadFile(CorePathHash("ds/test/two.core.stream"), nullptr, got, err));   // no decompressor for the compressed chunk
    RemoveTree(dir);
}

void CoreObjects() {
    std::uint32_t mips = 0;
    const std::vector<std::uint8_t> chain = Rgba8Chain(8, 8, 1, mips);
    std::vector<std::uint8_t> core = TextureObject(0x10, kRGBA8, 8, 8, 1, chain, mips);
    const std::vector<std::uint8_t> s = SetObject({{0x10, 0x86211101u}});
    core.insert(core.end(), s.begin(), s.end());
    std::vector<CoreObject> objs;
    std::string err;
    CHECK(ReadCoreObjects(core, objs, err) && objs.size() == 2);
    TextureInfo t;
    CHECK(ParseTexture(objs[0].body, objs[0].size, t, err));
    CHECK(t.width == 8 && t.mips == 4 && t.format == kRGBA8 && t.srgb == 1 && t.flags == 0xA9 && t.embedded_size == chain.size());
    CHECK(t.guid == "13121110-1514-1716-1819-1a1b1c1d1e1f");
    CHECK(CheckTexture(t, 0, err));
    const std::uint8_t* d = nullptr;
    std::size_t n = 0;
    CHECK(MipData(t, {}, 1, d, n, err) && n == 4 * 4 * 4 && d == t.embedded.data() + 8 * 8 * 4);
    CHECK(!MipData(t, {}, 4, d, n, err));
    TextureSetInfo ts;
    CHECK(ParseTextureSet(objs[1].body, objs[1].size, ts, err) && ts.entries.size() == 1 && ts.entries[0].packing == 0x86211101u &&
          ts.entries[0].texture_guid == t.guid);
    // A corrupt 'remaining' and a truncated core are refused.
    const std::vector<std::uint8_t> bad = TextureObject(0x10, kRGBA8, 8, 8, 1, chain, mips, true);
    CHECK(ReadCoreObjects(bad, objs, err) && !ParseTexture(objs[0].body, objs[0].size, t, err));
    std::vector<std::uint8_t> cut(core.begin(), core.end() - 3);
    CHECK(!ReadCoreObjects(cut, objs, err));
    // The chain must add up.
    TextureInfo wrong = t;
    wrong.mips = 3;
    CHECK(!CheckTexture(wrong, 0, err));
    wrong = t;
    wrong.width = 5000;
    CHECK(!CheckTexture(wrong, 0, err));
}

void Png() {
    const Image src = Gradient(37, 21);
    std::vector<std::uint8_t> png;
    CHECK(EncodePng(src, PngLayout::rgb24, png));
    std::uint32_t w = 0, h = 0;
    CHECK(PngSize(png.data(), png.size(), w, h) && w == 37 && h == 21);
    Image back;
    CHECK(DecodePng(png.data(), png.size(), back));
    CHECK(Psnr(src, back, 0x7) >= 99.0 && back.At(0, 0)[3] == 255);
    CHECK(EncodePng(src, PngLayout::gray8, png));
    CHECK(DecodePng(png.data(), png.size(), back) && back.At(5, 5)[0] == src.At(5, 5)[0] && back.At(5, 5)[1] == src.At(5, 5)[0]);
    CHECK(EncodePng(src, PngLayout::rgba32, png) && DecodePng(png.data(), png.size(), back) && back.rgba == src.rgba);
    const std::uint8_t junk[30] = {1, 2, 3};
    CHECK(!DecodePng(junk, sizeof(junk), back) && !PngSize(junk, sizeof(junk), w, h));
}

// ---- The whole extraction on a synthetic game folder -------------------------------------------------------
void Extraction() {
    const std::wstring root = TempDir(L"extract");
    RemoveTree(root);
    CreateDirectoryW(TempDir(L"").c_str(), nullptr);
    CreateDirectoryW(root.c_str(), nullptr);
    const std::wstring game = root + L"\\game", out = root + L"\\out";
    CreateDirectoryW(game.c_str(), nullptr);
    CreateDirectoryW((game + L"\\data").c_str(), nullptr);
    {
        HANDLE f = CreateFileW((game + L"\\ds.exe").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        DWORD put = 0;
        WriteFile(f, "MZ", 2, &put, nullptr);
        CloseHandle(f);
    }
    // Her suit (Colour + Roughness, Reflectance + AO) and her boots (Colour; AO, Roughness, Reflectance), 16x16 RGBA8.
    auto tex = [](std::uint8_t guid, std::uint8_t srgb, std::uint8_t seed) {
        std::uint32_t mips = 0;
        const std::vector<std::uint8_t> chain = Rgba8Chain(16, 16, seed, mips);
        return TextureObject(guid, kRGBA8, 16, 16, srgb, chain, mips);
    };
    std::vector<std::uint8_t> suit = tex(0x10, 1, 3);
    std::vector<std::uint8_t> t2 = tex(0x30, 0, 9);
    suit.insert(suit.end(), t2.begin(), t2.end());
    std::vector<std::uint8_t> s = SetObject({{0x10, 0x86211101u}, {0x30, 0x85241404u}});
    suit.insert(suit.end(), s.begin(), s.end());
    std::vector<std::uint8_t> boots = tex(0x50, 1, 20);
    t2 = tex(0x70, 0, 40);
    boots.insert(boots.end(), t2.begin(), t2.end());
    s = SetObject({{0x50, 0x80211101u}, {0x70, 0x80848685u}});
    boots.insert(boots.end(), s.begin(), s.end());
    CHECK(WriteArchive(game + L"\\data\\00.bin", {{kSuitSet, suit}, {kBootSet, boots}}));
    static const std::vector<ExtractJob> jobs = {{kOriginalDir, "Suit", kSuitSet, true}, {kOriginalDir, "Boots", kBootSet, true},
                                             {"Boots (Sam, reference)\\Porter boots", "Boots", kBootSet, false}};
    ExtractOptions o;
    o.game_dir = game;
    o.out_dir = out;
    o.previews = false;
    o.decompressor = FakeDecompress;
    o.jobs = &jobs;
    o.min_free_bytes = 1;
    std::vector<std::string> lines;
    o.emit = [&](const std::string& l) { lines.push_back(l); };
    ExtractStats st;
    std::string err;
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // The slot check: the synthetic textures are not the game's (other GUIDs and sizes), so the real rule refuses them
    // with a sentence and writes no manifest.
    CHECK(!RunExtract(o, st, err) && err.find("this Stranded Together version cannot use it") != std::string::npos);
    CHECK(!FileExists(out + L"\\.manifest.txt"));
    o.check_slots = false;
    // A folder that already holds someone else's files (and was never extracted into) is refused; nothing is deleted.
    {
        const std::wstring foreign = root + L"\\foreign";
        CreateDirectoryW(foreign.c_str(), nullptr);
        HANDLE f = CreateFileW((foreign + L"\\download.zip.partial").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        CloseHandle(f);
        ExtractOptions fo = o;
        fo.out_dir = foreign;
        CHECK(!RunExtract(fo, st, err) && err.find("already has other files") != std::string::npos);
        CHECK(FileExists(foreign + L"\\download.zip.partial") && !FileExists(foreign + L"\\README.txt"));
    }
    lines.clear();
    CHECK(RunExtract(o, st, err));
    if (!err.empty()) std::printf("  extract: %s\n", err.c_str());
    CHECK(st.sets_done == 3 && st.kept == 0 && !lines.empty() && lines.back().rfind("done ", 0) == 0);
    const std::wstring orig = out + L"\\Fragile porter suit\\Original\\";
    for (const wchar_t* f : {L"Suit - Colour.png", L"Suit - Roughness.png", L"Suit - Reflectance.png", L"Suit - Ambient occlusion.png",
                             L"Boots - Colour.png", L"Boots - Roughness.png", L"Boots - Reflectance.png", L"Boots - Ambient occlusion.png"})
        CHECK(FileExists(orig + f));
    CHECK((GetFileAttributesW((orig + L"Suit - Colour.png").c_str()) & FILE_ATTRIBUTE_READONLY) != 0);
    const std::wstring gold = out + L"\\Fragile porter suit\\16 Gold\\";
    CHECK(FileExists(gold + L"Suit - Colour.png") && FileExists(gold + L"Suit - Reflectance.png") && FileExists(gold + L"Sam's colour.png"));
    CHECK(!FileExists(gold + L"Suit - Ambient occlusion.png"));
    CHECK(FileHash(gold + L"Suit - Colour.png") == FileHash(orig + L"Suit - Colour.png"));   // a seed is a byte copy
    CHECK((GetFileAttributesW((gold + L"Suit - Colour.png").c_str()) & FILE_ATTRIBUTE_READONLY) == 0);
    CHECK(FileExists(out + L"\\Fragile boots\\09 Chiral boots\\Boots - Colour.png") && FileExists(out + L"\\Fragile boots\\09 Chiral boots\\Worn"));
    CHECK(FileExists(out + L"\\Boots (Sam, reference)\\Porter boots\\Boots - Roughness.png"));
    CHECK(FileExists(out + L"\\README.txt") && (GetFileAttributesW((out + L"\\.manifest.txt").c_str()) & FILE_ATTRIBUTE_HIDDEN) != 0);
    CHECK((GetFileAttributesW((out + L"\\README.txt").c_str()) & FILE_ATTRIBUTE_READONLY) != 0);   // a regenerated reference
    // The split: the roughness picture is the colour texture's alpha.
    Image rough, col;
    CHECK(ReadPngFile(orig + L"Suit - Roughness.png", rough) && rough.w == 16 && rough.At(1, 0)[0] == 13 && rough.At(1, 0)[2] == 13);
    CHECK(ReadPngFile(orig + L"Suit - Colour.png", col) && col.At(1, 0)[0] == 4 && col.At(1, 0)[2] == 199);
    EditScan scan;
    ScanEdits(out, true, scan);
    CHECK(scan.extracted && scan.table_ok && scan.edited.empty() && scan.colour_folders == 17 && scan.boot_folders == 10 && scan.problems.empty());

    // Edit one seed, delete another, rename a folder, drop a stray file; extract again.
    {
        HANDLE f = CreateFileW((gold + L"Suit - Colour.png").c_str(), FILE_APPEND_DATA, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        DWORD put = 0;
        WriteFile(f, "x", 1, &put, nullptr);
        CloseHandle(f);
    }
    const std::string edited_hash = FileHash(gold + L"Suit - Colour.png");
    DeleteFileW((out + L"\\Fragile porter suit\\03 Charcoal\\Suit - Roughness.png").c_str());
    CHECK(MoveFileW((out + L"\\Fragile porter suit\\09 Mustard").c_str(), (out + L"\\Fragile porter suit\\09 My yellow").c_str()));
    {
        HANDLE f = CreateFileW((out + L"\\Fragile porter suit\\09 My yellow\\notes.png").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        CloseHandle(f);
    }
    ScanEdits(out, false, scan);
    CHECK(EditSummary(scan) == "Edited: 16 Gold (Colour)");
    CHECK(scan.problems.size() == 1);   // notes.png is not used
    lines.clear();
    CHECK(RunExtract(o, st, err));
    CHECK(FileHash(gold + L"Suit - Colour.png") == edited_hash);   // never overwritten
    CHECK(FileExists(out + L"\\Fragile porter suit\\03 Charcoal\\Suit - Roughness.png"));   // re-seeded
    CHECK(!FileExists(out + L"\\Fragile porter suit\\09 Mustard"));   // the renamed folder is used, not re-created
    CHECK(FileExists(out + L"\\Fragile porter suit\\09 My yellow\\Suit - Colour.png") && FileExists(out + L"\\Fragile porter suit\\09 My yellow\\notes.png"));
    CHECK(st.sets_reused == 3 && st.kept > 0);
    bool kept_line = false;
    for (const std::string& l : lines) kept_line |= l == "kept Fragile porter suit\\16 Gold\\Suit - Colour.png";
    CHECK(kept_line);
    // A partial file of an interrupted run is removed by the next one; someone else's .partial files are not.
    {
        HANDLE f = CreateFileW((orig + L"Suit - Normal.png.partial").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        CloseHandle(f);
        CreateDirectoryW((out + L"\\My notes").c_str(), nullptr);
        f = CreateFileW((out + L"\\My notes\\sketch.psd.partial").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        CloseHandle(f);
    }
    // A reference file deleted by the user is regenerated (its set is no longer up to date).
    SetFileAttributesW((orig + L"Boots - Colour.png").c_str(), FILE_ATTRIBUTE_NORMAL);
    DeleteFileW((orig + L"Boots - Colour.png").c_str());
    CHECK(RunExtract(o, st, err));
    CHECK(!FileExists(orig + L"Suit - Normal.png.partial") && FileExists(orig + L"Boots - Colour.png"));
    CHECK(FileExists(out + L"\\My notes\\sketch.psd.partial"));
    CHECK(st.sets_done == 1 && st.sets_reused == 2);
    // A game update: the seed rows keep the first-written hash, so the edited file still counts as edited.
    Manifest m;
    {
        std::vector<std::uint8_t> b;
        HANDLE f = CreateFileW((out + L"\\.manifest.txt").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        LARGE_INTEGER sz{};
        GetFileSizeEx(f, &sz);
        b.resize(static_cast<std::size_t>(sz.QuadPart));
        DWORD got = 0;
        ReadFile(f, b.data(), static_cast<DWORD>(b.size()), &got, nullptr);
        CloseHandle(f);
        CHECK(ParseManifest(std::string(b.begin(), b.end()), m, err));
    }
    const ManifestRow* seed = m.Find("Fragile porter suit\\16 Gold\\Suit - Colour.png");
    const ManifestRow* org = m.Find("Fragile porter suit\\Original\\Suit - Colour.png");
    CHECK(seed && org && seed->kind == "seed" && seed->sha256 == org->sha256 && seed->slot == org->slot);
    // Not enough disk space: refused with both numbers.
    o.min_free_bytes = ~0ull >> 1;
    CHECK(!RunExtract(o, st, err) && err.find("MB free") != std::string::npos);
    // No game archives: a sentence, nothing written.
    o.min_free_bytes = 1;
    o.game_dir = root + L"\\nogame";
    o.out_dir = root + L"\\out2";
    CHECK(!RunExtract(o, st, err) && !err.empty());
    CHECK(PathInside(out + L"\\a\\b", out) && PathInside(out, out + L"\\") && !PathInside(root + L"\\outside", out) && !PathInside(out + L"x", out));
    if (SUCCEEDED(co)) CoUninitialize();
    RemoveTree(root);
}

}  // namespace

int main() {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Formats();
    Codecs();
    Mips();
    Packing();
    Slots();
    ManifestText();
    Names();
    Hashes();
    ArchiveReader();
    CoreObjects();
    Png();
    Extraction();
    if (SUCCEEDED(co)) CoUninitialize();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
