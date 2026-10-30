#pragma once
#include "frame.h"

// Procedurally drawn, anti-aliased target sprites (replaces Purple_Dot.png, so
// the target no longer depends on an asset being found next to the exe).
//
// Both sprites put the visible disc/ring edge at kSpriteEdge of the quad's
// half-width, so a ring whose quad is as wide as the dot's lands exactly on the
// dot's rim when it converges.

namespace sprites {

inline constexpr float kSpriteEdge = 0.80f;

// Fixation dot: flat violet disc with a dark centre ring and a white pip
// marking the exact fixation point. Small texture on purpose: the dot is only
// ~1.4° wide, and a large source would alias when minified without mipmaps.
const Image& dot();

// Attention ring: a flat light stroke that converges onto the dot.
const Image& ring();

// Motion trail: a horizontal comet streak, tail at the left edge (u = 0,
// transparent and narrow) to the head at the right edge (u = 1). The quad is
// stretched to the trail length and rotated along the motion; its head end sits
// under the dot, so the trail is never wider than the dot's visible disc.
const Image& trail();
inline constexpr float kTrailWidth = 0.62f;  // trail thickness / visible dot diameter

}  // namespace sprites
