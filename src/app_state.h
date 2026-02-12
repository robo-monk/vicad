#ifndef VICAD_APP_STATE_H_
#define VICAD_APP_STATE_H_

#include <cmath>

#include "edge_detection.h"
#include "face_detection.h"

namespace vicad_app {

struct Vec3 {
  float x;
  float y;
  float z;
};

struct Vec2 {
  float x;
  float y;
};

struct FaceSelectState {
  bool enabled;
  bool dirty;
  float angleThresholdDeg;
  int hoveredRegion;
  int selectedRegion;
  vicad::FaceDetectionResult faces;
};

struct EdgeSelectState {
  bool enabled;
  bool dirtyTopology;
  bool dirtySilhouette;
  float sharpAngleDeg;
  int hoveredEdge;
  int selectedEdge;
  int hoveredChain;
  int selectedChain;
  vicad::EdgeDetectionResult edges;
  vicad::SilhouetteResult silhouette;
};

struct CameraBasis {
  Vec3 forward;
  Vec3 right;
  Vec3 up;
};

struct DimensionRenderContext {
  Vec3 eye;
  CameraBasis camera;
  float fovDegrees;
  int viewportHeight;
  float arrowPixels;
};

inline Vec3 add(const Vec3 &a, const Vec3 &b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 mul(const Vec3 &v, float s) { return {v.x * s, v.y * s, v.z * s}; }
inline Vec3 sub(const Vec3 &a, const Vec3 &b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 cross(const Vec3 &a, const Vec3 &b) {
  return {a.y * b.z - a.z * b.y,
          a.z * b.x - a.x * b.z,
          a.x * b.y - a.y * b.x};
}
inline Vec3 normalize(const Vec3 &v) {
  const float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
  if (l < 1e-8f) return {0.0f, 0.0f, 1.0f};
  return {v.x / l, v.y / l, v.z / l};
}
inline float dot(const Vec3 &a, const Vec3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float clampf(float v, float lo, float hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}
inline Vec2 add2(const Vec2 &a, const Vec2 &b) { return {a.x + b.x, a.y + b.y}; }
inline Vec2 sub2v(const Vec2 &a, const Vec2 &b) { return {a.x - b.x, a.y - b.y}; }
inline Vec2 mul2(const Vec2 &v, float s) { return {v.x * s, v.y * s}; }
inline float dot2(const Vec2 &a, const Vec2 &b) { return a.x * b.x + a.y * b.y; }
inline float length2(const Vec2 &v) { return std::sqrt(dot2(v, v)); }
inline Vec2 normalize2(const Vec2 &v) {
  const float l = length2(v);
  if (l < 1e-8f) return {1.0f, 0.0f};
  return {v.x / l, v.y / l};
}
inline Vec2 perp2(const Vec2 &v) { return {-v.y, v.x}; }
inline Vec3 vec3_from_2d(const Vec2 &v, float z) { return {v.x, v.y, z}; }

}  // namespace vicad_app

#endif  // VICAD_APP_STATE_H_
