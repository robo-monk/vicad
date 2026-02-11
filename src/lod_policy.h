#ifndef VICAD_LOD_POLICY_H_
#define VICAD_LOD_POLICY_H_

#include <cstdint>

#include "manifold/manifold.h"

namespace vicad {

// Profile-driven level of detail for replayed geometry construction.
enum class LodProfile : uint8_t {
  Draft = 0,
  Model = 1,
  Export3MF = 2,
};

// Global defaults in one clear place, in scene units.
extern const double kLodToleranceDraft;
extern const double kLodToleranceModel;
extern const double kLodToleranceExport3MF;

struct ReplayPostprocessPolicy {
  // Future-facing hook: when enabled, run RefineToTolerance() after replay.
  bool refineToToleranceEnabled = false;
  double refineTolerance = 0.0;
};

struct ReplayLodPolicy {
  LodProfile profile = LodProfile::Model;
  ReplayPostprocessPolicy postprocess = {};
};

double LodToleranceForProfile(LodProfile profile);

// Auto-derived circular tessellation from profile tolerance.
int AutoCircularSegments(double radius, LodProfile profile);
int AutoCircularSegmentsForRevolve(double radius, double revolveDegrees,
                                   LodProfile profile);

manifold::Manifold ApplyReplayPostprocess(
    const manifold::Manifold& input,
    const ReplayPostprocessPolicy& postprocess);

}  // namespace vicad

#endif  // VICAD_LOD_POLICY_H_
