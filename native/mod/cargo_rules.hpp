// The carried cargo on the other screen, the pure part (docs/real-partner-body.md section 39): which carried pieces the
// sender sends and with which model value, and what the receiver draws for that value. No game memory, no Windows:
// unit-tested in tests/cargo_rules_tests.cpp. The game side is partner_gear.cpp (ReadItems, PropForKind) and
// bag_model.cpp (the exact model).
//
// Background (round 14, the user's "fragile still cargo is invisible on host screen. its visible on her own screen"):
// the two hip crates the game draws on Sam's waist slots (carrier slot types 4 / 5) were read and dropped since v0.8. The
// left hip carries a weapon case (case type 2: the game's wsm0 / wmd0 / wla0 weapon case, a dark caged crate), the right
// hip the folding ladder (case type 12: flad, the brown folded ladder). Neither has a cutscene prop; v0.8 drew a white
// delivery case at the hip, which the player never wears, and was taken out. The game draws a carried piece with one of
// the 31 SkinnedModelResources of its baggage config (DSBaggage2Config.ModelResources, EDSBaggage2Model order); round 14
// draws exactly that model on the partner (bag_model.cpp).
//
// The wire: STC1 cargo items keep their model byte. 0..30 is EDSBaggage2Model drawn with a cutscene case prop (unchanged since
// v0.7); kExact | m (0x80..0x9E) is "the baggage config's own model m". A receiver older than round 14 maps any model of
// 31 or more but 0xFF to no prop, so it draws nothing for these (never a white case at the hip).
#pragma once
#include <cstdint>

namespace st::cargorules {

inline constexpr std::uint8_t kNone = 0xFF;   // no model: not drawn
inline constexpr std::uint8_t kExact = 0x80;  // model byte flag: the baggage config's own model (low bits: EDSBaggage2Model)
inline constexpr unsigned kModels = 31;       // EDSBaggage2Model 0..30 (the baggage config's ModelResources)

// The carrier's slot types the hips hold (left / right waist).
inline bool IsWaist(std::uint8_t slot_type) { return slot_type == 4 || slot_type == 5; }

// The list item's attribute, case type and volume -> EDSBaggage2Model (the model the game draws), kNone when unknown.
inline std::uint8_t ModelFromCase(std::uint8_t attr, std::uint8_t kase, std::uint8_t vol) {
    const std::uint8_t size = vol > 2 ? 2 : vol;  // S, M, L (LL: L)
    switch (kase) {
        case 0: return static_cast<std::uint8_t>((attr == 1 ? 4 : 1) + size);  // Normal (Personal: 4..6)
        case 1: return 13;                                                       // LiquidOnly: barrel
        case 2: return static_cast<std::uint8_t>(15 + size);                     // Weapon cases wsm0 / wmd0 / wla0
        case 9: return static_cast<std::uint8_t>(vol >= 3 ? 21 : 18 + size);     // Material
        case 10: return 11;                                                      // Cart
        case 12: return 14;                                                      // Ladder (flad)
        case 17: return 27;                                                      // SmallDelicate
        case 18: return 30;                                                      // ToxicGas
        default: return kNone;
    }
}

// Deliveries (Normal, LiquidOnly, Material, Delicate, SmallDelicate, ToxicGas): drawn anywhere with a cutscene case prop.
inline bool DeliveryCase(std::uint8_t kase) {
    switch (kase) {
        case 0: case 1: case 9: case 13: case 17: case 18: return true;
        default: return false;
    }
}

// What the game draws at a hip, for the exact model: the weapon case (case 2) and the folding ladder (case 12). Equipment
// (case 4) and anything else at the hip: kNone (no model known).
inline std::uint8_t WaistModel(std::uint8_t kase, std::uint8_t vol) {
    if (kase != 2 && kase != 12) return kNone;
    return ModelFromCase(0, kase, vol);
}

// The model byte the sender puts in the STC1 cargo item for a carried piece (kNone: not sent). `waist_on`: session.ini
// waist_cargo (default on since round 15, section 39.6).
inline std::uint8_t SendModel(std::uint8_t slot_type, std::uint8_t attr, std::uint8_t kase, std::uint8_t vol, bool waist_on) {
    if (DeliveryCase(kase)) return ModelFromCase(attr, kase, vol);
    if (!waist_on || !IsWaist(slot_type)) return kNone;
    const std::uint8_t m = WaistModel(kase, vol);
    return m == kNone ? kNone : static_cast<std::uint8_t>(kExact | m);
}

// The receiver: the exact model a cargo item names (0..30), or -1 (a cutscene-prop model, or none).
inline int ExactModel(std::uint8_t model) {
    if (model == kNone || !(model & kExact)) return -1;
    const unsigned m = model & 0x7Fu;
    return m < kModels ? static_cast<int>(m) : -1;
}

// What the receiver draws for a cargo item's model byte (partner_gear.cpp PropForKind). `waist_on`: session.ini waist_cargo.
enum class Draw : std::uint8_t {
    none,       // nothing: waist_cargo off for an exact model, or a value no receiver knows (31..0xFE but an exact model)
    cutscene,   // 0..30: the cutscene case prop of that EDSBaggage2Model (v0.7)
    unknown,    // kNone: the sender knew no model: the generic M case (v0.7)
    exact,      // kExact | m: the baggage config's own model m (bag_model.cpp)
};
inline Draw ReceiverDraw(std::uint8_t model, bool waist_on) {
    if (ExactModel(model) >= 0) return waist_on ? Draw::exact : Draw::none;
    if (model < kModels) return Draw::cutscene;
    return model == kNone ? Draw::unknown : Draw::none;
}

// A short name of an EDSBaggage2Model for the log.
inline const char* ModelName(unsigned m) {
    switch (m) {
        case 14: return "folding ladder (flad)";
        case 15: return "weapon case S (wsm0)";
        case 16: return "weapon case M (wmd0)";
        case 17: return "weapon case L (wla0)";
        default: return m < kModels ? "baggage model" : "?";
    }
}

}  // namespace st::cargorules
