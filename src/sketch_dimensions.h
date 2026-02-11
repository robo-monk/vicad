#ifndef VICAD_SKETCH_DIMENSIONS_H_
#define VICAD_SKETCH_DIMENSIONS_H_

#include <cstdint>
#include <string>
#include <vector>

#include "manifold/cross_section.h"

namespace vicad {

enum class SketchPrimitiveKind : uint32_t {
  Unknown = 0,
  Circle = 1,
  Rect = 2,
  RegularPolygon = 3,
  IrregularPolygon = 4,
  Point = 5,
};

struct SketchLineDim {
  manifold::vec2 a = manifold::vec2(0.0, 0.0);
  manifold::vec2 b = manifold::vec2(0.0, 0.0);
  double value = 0.0;
};

struct SketchTextSummary {
  manifold::vec2 anchor = manifold::vec2(0.0, 0.0);
  std::string text;
};

struct SketchDimensionEntity {
  enum class Kind : uint8_t {
    LineDim = 0,
    TextSummary = 1,
  };

  Kind kind = Kind::LineDim;
  SketchLineDim line;
  SketchTextSummary summary;
};

struct SketchDimensionModel {
  SketchPrimitiveKind primitive = SketchPrimitiveKind::Unknown;
  std::vector<manifold::vec2> logicalVertices;
  manifold::vec2 anchor = manifold::vec2(0.0, 0.0);

  bool hasRectSize = false;
  double rectWidth = 0.0;
  double rectHeight = 0.0;

  bool hasCircleRadius = false;
  double circleRadius = 0.0;

  bool hasFillet = false;
  double filletRadius = 0.0;

  bool regularPolygon = false;
  uint32_t polygonSides = 0;

  std::vector<SketchDimensionEntity> entities;
};

struct OpTraceEntry {
  uint16_t opcode = 0;
  std::string name;
  uint32_t outId = 0;
  std::vector<double> args;
};

const char *SketchPrimitiveKindName(SketchPrimitiveKind kind);

}  // namespace vicad

#endif  // VICAD_SKETCH_DIMENSIONS_H_
