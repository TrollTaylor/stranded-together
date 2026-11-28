// Name tags in the overlay (docs/real-partner-body.md section 10): the partner's name drawn
// above their head, projected through the game's own camera every presented frame.
//
//   game thread  --Publish(Frame)-->  [locked copy]  --inside Present-->  Draw
//
// The game side (nametags.cpp) decides WHAT and WHETHER (name, world anchor, the game-state
// gates) and registers the camera reader; the overlay reads the camera view at Present (fault-
// free, through the reader) so the tag follows the frame being shown, projects and draws. With
// no reader registered (the overlay harness) nothing is ever drawn.
#pragma once
#include <cstddef>
#include <cstdint>

#include "nametag_math.hpp"

struct ImDrawList;

namespace st::overlay::tags {

inline constexpr unsigned kMaxTags = 2;

struct Tag {
    double anchor[3] = {};   // world point the label's bottom centre sits on (above the head)
    char text[96] = {};      // UTF-8, sanitized
};

struct Frame {
    bool show = false;            // all gates clear (game thread)
    std::uintptr_t view = 0;      // the game's camera view object (passed to the reader)
    unsigned count = 0;
    Tag tags[kMaxTags];
};

// Reads the camera view snapshot of `view` (game memory, fault-free). False: unusable.
using ViewReader = bool (*)(std::uintptr_t view, nametag::View* out) noexcept;
void SetViewReader(ViewReader reader) noexcept;

// Game thread, every tick while the feature is on (a frame older than 500 ms is not drawn).
void Publish(const Frame& f) noexcept;

// Round 30 BT S1 (docs/enemy-sync.md section 24): the partner's BT trouble as small in-world marks, published by bt_markers.cpp
// beside the name tags and drawn through the same camera reader. No text: a diamond over a gazer sensing the partner, a dark ring
// on the ground round the partner while BTs hunt them, a circle with a health bar over the catcher they fight.
inline constexpr unsigned kMaxBtMarks = 8;
enum BtMarkKind : std::uint8_t { kBtGazer = 1, kBtTar = 2, kBtCatcher = 3 };
struct BtMark {
    std::uint8_t kind = 0;
    double anchor[3] = {};  // gazer: over its head; tar: the ground under the partner; catcher: over it
    float radius = 0;       // tar: the ring's radius (m)
    float hp = -1;          // catcher: 0..100 (percent), else -1
    bool gold = false;      // tar: a golden hunt (the ring drawn gold)
};
struct BtFrame {
    bool show = false;
    std::uintptr_t view = 0;
    unsigned count = 0;
    BtMark marks[kMaxBtMarks];
};
void PublishBt(const BtFrame& f) noexcept;

// Render thread: a fresh frame (names or BT marks) wants drawing (the overlay keeps its renderer for it).
bool Wanted() noexcept;
// Render thread, between ImGui::NewFrame and Render. w, h: back buffer; ui_scale: the overlay's.
void Draw(ImDrawList* dl, float w, float h, float ui_scale) noexcept;

struct Stats {
    unsigned long published = 0;      // frames published
    unsigned long drawn_frames = 0;   // presented frames with at least one tag drawn
    unsigned long view_failures = 0;  // reader said no (torn twice, wrong vtable, not perspective)
    unsigned long offscreen = 0;      // tags not drawn: behind the camera / outside the view / too far
    float last_x = 0, last_y = 0;     // last drawn tag: anchor in pixels
    float last_w = 0, last_h = 0;     // its box (pixels)
    float last_dist = 0;              // metres
    float last_size = 0;              // text px
    bool last_fitted = false;         // letter-/pillarboxed viewport
    // round 30 BT marks
    unsigned long bt_published = 0, bt_drawn_frames = 0;
    unsigned bt_last_drawn = 0, bt_last_gazer = 0, bt_last_tar = 0, bt_last_catcher = 0;  // the last frame that drew a mark
};
Stats GetStats() noexcept;

}  // namespace st::overlay::tags
