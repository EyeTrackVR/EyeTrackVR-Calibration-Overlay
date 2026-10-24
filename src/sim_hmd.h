#pragma once
#include "fov.h"

#include <string>
#include <vector>

// Synthetic headset descriptions for the simulator backend and unit tests.
// Approximate per-eye visible FOVs (degrees) with lens-shaped visibility
// polygons; they cover the shapes that matter for target placement: strongly
// down-heavy (Beyond-like), wide symmetric (Index), compact pancake (Quest 3),
// canted wide (Pimax), round-lens (Vive) and narrow headsets.
std::vector<std::string> sim_hmd_names();
bool sim_hmd(const std::string& name, EyeGeom out[2]);
