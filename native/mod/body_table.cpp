#include "body_table.hpp"

#include <cctype>
#include <cstring>

namespace st::bodies {
namespace {

// docs/game-data-index.md "Cutscene actor bodies" (decoded with tools/re/ds_archive.py).
// Ids are part of the wire format (STJ1 body_id): append only, never renumber.
// The last field is `offered`: set it only after a body passed a hidden tour run (spawned,
// validated, posed; docs/real-partner-body.md section 1). Ids 1-5: runs 184029 and 184259; all 16:
// tools/testenv/scenarios/real-body-tour-all.txt, runs 20261116-194441 and -200116.
constexpr Body kBodies[] = {
    {1, "sam", "Sam (porter suit)", "ds/entities/cinematics/demo_sam_clean", "514b5705-aaf0-a447-a82e-0bb83b855201", true},
    {2, "porter", "Bridges porter", "ds/entities/cinematics/demo_bdv_body_suits", "d210ec9c-8f91-bc43-816d-40173a5ad3cc", true},
    {3, "fragile", "Fragile", "ds/entities/cinematics/demo_fgl_new", "336f4328-1381-4947-8e6d-7def29e654bd", true},
    {4, "diehardman", "Die-Hardman", "ds/entities/cinematics/demo_dhm_body_new", "3e20c5a3-1829-1c46-a7af-12b8fa56a2d8", true},
    {5, "cliff", "Cliff Unger", "ds/entities/cinematics/demo_fth_father_new02", "dcf18e42-aa86-6f45-85bd-db75052a185f", true},
    {6, "porter_plain", "Bridges porter (no suit)", "ds/entities/cinematics/demo_bdv_body_def", "ab317812-6fce-ec44-874c-26532bfe3ba0", true},
    {7, "deadman", "Deadman", "ds/entities/cinematics/demo_ddm_new", "4f44c8dd-c7ab-974c-9b55-fbeee6f33039", true},
    {8, "heartman", "Heartman", "ds/entities/cinematics/demo_htm_body_def", "807b739b-397a-8741-bce1-6255fa332a0b", true},
    {9, "mama", "Mama", "ds/entities/cinematics/demo_mam_body_def", "b164763f-300b-dd47-ba13-ececde97d6e4", true},
    {10, "lockne", "Lockne", "ds/entities/cinematics/demo_lkn_body_def", "59612099-ba7c-f94a-9b8a-18032fac78e2", true},
    {11, "amelie", "Amelie", "ds/entities/cinematics/demo_aml_body_new", "921db5fb-6fb7-f844-9a54-73075865ab29", true},
    {12, "higgs", "Higgs", "ds/entities/cinematics/demo_hgs_body_def_new", "17cbc331-ea36-1043-a9f5-9871f6dcddde", true},
    {13, "guard", "Bridges guard", "ds/entities/cinematics/demo_bgs_body_def_v01", "73817e25-9845-d443-b209-b3065497fb6e", true},
    {14, "medic", "Bridges medic", "ds/entities/cinematics/demo_bme_body_def", "6b9fe7c0-630c-1442-a34f-717850672b80", true},
    {15, "sam_mule", "Sam (MULE suit)", "ds/entities/cinematics/demo_sam_body_suitmule_demo_new", "ee0118b9-947a-fa46-b38d-bbacb6675c85", true},
    {16, "fragile_suit", "Fragile (porter suit)", "ds/entities/cinematics/demo_fgl_body_suiti", "dbbb2d95-8583-f046-a5aa-fc7b0dafa8f6", true},
};
constexpr std::size_t kCount = sizeof(kBodies) / sizeof(kBodies[0]);

int Hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

std::size_t Count() { return kCount; }
const Body& At(std::size_t i) { return kBodies[i < kCount ? i : 0]; }
const Body& Default() { return kBodies[0]; }

const Body* FindByKey(const char* key) {
    if (!key || !*key) return nullptr;
    for (const Body& b : kBodies) {
        const char* a = b.key;
        const char* k = key;
        while (*a && *k && std::tolower(static_cast<unsigned char>(*a)) == std::tolower(static_cast<unsigned char>(*k))) ++a, ++k;
        if (!*a && !*k) return &b;
    }
    return nullptr;
}

const Body* FindById(std::uint8_t id) {
    for (const Body& b : kBodies)
        if (b.id == id) return &b;
    return nullptr;
}

bool IsOffered(std::uint8_t id) {
    const Body* b = FindById(id);
    return b && b->offered;
}

std::size_t OfferedCount() {
    std::size_t n = 0;
    for (const Body& b : kBodies) n += b.offered ? 1 : 0;
    return n;
}

const Body& OfferedAt(std::size_t i) {
    for (const Body& b : kBodies)
        if (b.offered && i-- == 0) return b;
    return Default();
}

bool ParseGuid(const char* text, std::uint8_t (&out)[16]) {
    if (!text || std::strlen(text) != 36) return false;
    std::uint8_t raw[16] = {};
    unsigned n = 0;
    for (unsigned i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (text[i] != '-') return false;
            continue;
        }
        const int hi = Hex(text[i]), lo = Hex(text[i + 1]);
        if (hi < 0 || lo < 0) return false;
        raw[n++] = static_cast<std::uint8_t>(hi * 16 + lo);
        ++i;
    }
    if (n != 16) return false;
    // bytes_le: Data1 (4), Data2 (2), Data3 (2) little-endian, Data4 (8) as written.
    const std::uint8_t order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    for (unsigned i = 0; i < 16; ++i) out[i] = raw[order[i]];
    return true;
}

}  // namespace st::bodies
