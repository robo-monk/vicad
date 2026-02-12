#include "lod_policy.h"

#include <algorithm>
#include <cmath>

namespace vicad {

// Balanced defaults selected for profile-driven modelling/export.
const double kLodToleranceDraft = 0.1;
const double kLodToleranceModel = 0.01;
const double kLodToleranceExport3MF = 0.0001;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kMinCircularSegments = 4;
constexpr int kMaxCircularSegments = 8192;

int round_up_to_multiple_of_four(int n) {
  if (n <= 0) return 0;
  return ((n + 3) / 4) * 4;
}

int circular_segments_for_radius_and_tolerance(double radius, double tolerance) {
  radius = std::abs(radius);
  if (!std::isfinite(radius) || radius <= 1e-12) return kMinCircularSegments;

  if (!std::isfinite(tolerance) || tolerance <= 0.0) {
    tolerance = kLodToleranceModel;
  }
  tolerance = std::max(tolerance, 1e-9);

  if (tolerance >= radius) return kMinCircularSegments;

  // Sagitta error bound for a circle approximated by n segments:
  // sagitta = r * (1 - cos(pi / n)) <= tolerance
  const double cos_arg = std::clamp(1.0 - tolerance / radius, -1.0, 1.0);
  const double theta = std::acos(cos_arg);
  if (!std::isfinite(theta) || theta <= 1e-9) return kMaxCircularSegments;

  int n = (int)std::ceil(kPi / theta);
  n = std::clamp(n, kMinCircularSegments, kMaxCircularSegments);
  return round_up_to_multiple_of_four(n);
}

}  // namespace

double LodToleranceForProfile(LodProfile profile) {
  switch (profile) {
    case LodProfile::Draft:
      return kLodToleranceDraft;
    case LodProfile::Export3MF:
      return kLodToleranceExport3MF;
    case LodProfile::Model:
    default:
      return kLodToleranceModel;
  }
}

int AutoCircularSegments(double radius, LodProfile profile) {
  return circular_segments_for_radius_and_tolerance(
      radius, LodToleranceForProfile(profile));
}

int AutoCircularSegmentsForRevolve(double radius, double revolveDegrees,
                                   LodProfile profile) {
  const int full = AutoCircularSegments(radius, profile);
  if (!std::isfinite(revolveDegrees) || revolveDegrees <= 0.0) return 3;
  const double clamped = std::min(revolveDegrees, 360.0);
  const int scaled = (int)std::ceil((double)full * clamped / 360.0);
  return std::max(3, scaled);
}

manifold::Manifold ApplyReplayPostprocess(
    const manifold::Manifold& input,
    const ReplayPostprocessPolicy& postprocess) {
  if (!postprocess.refineToToleranceEnabled) return input;
  if (!std::isfinite(postprocess.refineTolerance) ||
      postprocess.refineTolerance <= 0.0) {
    return input;
  }
  return input.RefineToTolerance(postprocess.refineTolerance);
}

}  // namespace vicad
