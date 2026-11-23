// Stranded Together textures: which pictures Fragile wears for one suit colour folder (docs/textures.md "Ready-made
// colours"). Pure, header-only: the mod (texture_looks.cpp) and the extractor (extract_plan.cpp) decide with it, the
// tests check it.
//
// Every picture of a colour folder the mod reads (the six names of Original\: Colour, Reflectance, Roughness, Ambient
// occlusion, Normal, Mask) is missing, unedited or edited. Unedited: byte for byte what the extractor wrote in that
// folder (the manifest's seed row of the folder's NUMBER and that file name: the user may rename the rest), or, without
// a seed row, her Original's bytes. With ready-made colours on (session.ini textures_defaults=1, the default) and a
// colour 1..16:
//   - nothing edited: the folder is READY-MADE. She wears the colour the mod makes itself from Original\ (whatever
//     the unedited files are: copies of a v0.24 extraction, an older generator's output or the current one).
//   - any picture edited (also only an Ambient occlusion, Normal or Mask picture): the folder is YOURS. She wears
//     exactly its pictures, Original\ for a missing one; a slot whose inputs all equal Original\ is her own. A slot of a
//     yours folder with no edited picture may be the ready-made slot byte for byte (the folder still holds this
//     generator's unedited pictures): the mod builds both and, only when the two blobs are the same, uses the ready-made
//     one (the link then sends it as a recipe, not as 7 MB). Never decided from versions alone.
// Colour 0 (00 Default) or ready-made colours off: the rule of v0.24 (a slot with an edited picture is worn, else her
// own suit).
#pragma once
#include "suit_defaults.hpp"

namespace st::tex::readymade {

enum class PictureState { missing, unedited, edited };
enum class FolderState { ready_made, yours };
enum class SlotSource {
    own,          // her own textures (nothing bound for this slot)
    ready_made,   // the generator's slot (from Original\ pixels)
    files,        // built from the folder's files (Original\ for a missing one; left out when all equal Original\)
};
// The pictures of a suit colour folder the mod reads, in this order (extract_plan.cpp IsUsableSuitName).
enum Picture : int { kColourPic = 0, kReflectancePic, kRoughnessPic, kAoPic, kNormalPic, kMaskPic, kPictures };
inline constexpr const char* kPictureNames[kPictures] = {"Suit - Colour.png",           "Suit - Reflectance.png", "Suit - Roughness.png",
                                                         "Suit - Ambient occlusion.png", "Suit - Normal.png",      "Suit - Mask.png"};
inline constexpr int kSlotS0 = 0;   // Colour RGB + Roughness A
inline constexpr int kSlotS1 = 1;   // Reflectance RGB + Ambient occlusion A
inline constexpr int kSlotS2 = 2;   // Normal RG
inline constexpr int kSlotS3 = 3;   // Mask RGB

// The pictures (bit = 1 << Picture) a suit slot is packed from (tex_slots.hpp).
inline unsigned SlotPictures(int slot) {
    switch (slot) {
    case kSlotS0: return (1u << kColourPic) | (1u << kRoughnessPic);
    case kSlotS1: return (1u << kReflectancePic) | (1u << kAoPic);
    case kSlotS2: return 1u << kNormalPic;
    case kSlotS3: return 1u << kMaskPic;
    default: return 0;
    }
}
// The generator maps (defaults::Map) a suit slot carries.
inline unsigned SlotMaps(int slot) {
    return slot == kSlotS0 ? (defaults::kColour | defaults::kRoughness) : slot == kSlotS1 ? static_cast<unsigned>(defaults::kReflectance) : 0u;
}
inline FolderState Classify(const PictureState p[kPictures]) {
    for (int i = 0; i < kPictures; ++i)
        if (p[i] == PictureState::edited) return FolderState::yours;
    return FolderState::ready_made;
}

inline bool SlotEdited(int slot, const PictureState p[kPictures]) {
    for (int i = 0; i < kPictures; ++i)
        if ((SlotPictures(slot) & (1u << i)) && p[i] == PictureState::edited) return true;
    return false;
}

inline SlotSource SourceFor(int colour, int slot, FolderState folder, bool defaults_on, const PictureState p[kPictures]) {
    const bool edited = SlotEdited(slot, p);
    if (!defaults_on || !defaults::HasDefault(colour)) return edited ? SlotSource::files : SlotSource::own;
    if (folder == FolderState::ready_made) return (SlotMaps(slot) & defaults::ChangedMaps(colour)) ? SlotSource::ready_made : SlotSource::own;
    return SlotSource::files;
}

// A slot of a yours folder that may equal the ready-made slot byte for byte (no edited picture, and the ready-made colour
// changes one of its maps): the mod compares the two blobs and uses the ready-made one only when they are the same.
inline bool MayBeReadyMade(int colour, int slot, FolderState folder, bool defaults_on, const PictureState p[kPictures]) {
    return defaults_on && defaults::HasDefault(colour) && folder == FolderState::yours && !SlotEdited(slot, p) &&
           (SlotMaps(slot) & defaults::ChangedMaps(colour)) != 0;
}

}  // namespace st::tex::readymade
