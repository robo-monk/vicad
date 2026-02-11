#include "sketch_dimensions.h"

namespace vicad {

const char *SketchPrimitiveKindName(SketchPrimitiveKind kind) {
  switch (kind) {
    case SketchPrimitiveKind::Circle:
      return "Circle";
    case SketchPrimitiveKind::Rect:
      return "Rect";
    case SketchPrimitiveKind::RegularPolygon:
      return "RegularPolygon";
    case SketchPrimitiveKind::IrregularPolygon:
      return "IrregularPolygon";
    case SketchPrimitiveKind::Point:
      return "Point";
    case SketchPrimitiveKind::Unknown:
    default:
      return "Unknown";
  }
}

}  // namespace vicad
