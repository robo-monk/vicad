#include "op_decoder.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_set>
#include <vector>

#include "ipc_protocol.h"

namespace vicad {

namespace {

struct Affine2 {
  double a = 1.0;
  double b = 0.0;
  double c = 0.0;
  double d = 1.0;
  double tx = 0.0;
  double ty = 0.0;
};

manifold::vec2 apply_affine(const Affine2 &t, const manifold::vec2 &p) {
  return manifold::vec2(t.a * p.x + t.b * p.y + t.tx,
                        t.c * p.x + t.d * p.y + t.ty);
}

Affine2 translation2(double x, double y) {
  Affine2 t;
  t.tx = x;
  t.ty = y;
  return t;
}

Affine2 rotation2(double degrees) {
  const double r = degrees * 3.14159265358979323846 / 180.0;
  const double c = std::cos(r);
  const double s = std::sin(r);
  Affine2 t;
  t.a = c;
  t.b = -s;
  t.c = s;
  t.d = c;
  return t;
}

double polygon_area(const std::vector<manifold::vec2> &poly) {
  if (poly.size() < 3) return 0.0;
  double acc = 0.0;
  for (size_t i = 0; i < poly.size(); ++i) {
    const manifold::vec2 &a = poly[i];
    const manifold::vec2 &b = poly[(i + 1) % poly.size()];
    acc += a.x * b.y - b.x * a.y;
  }
  return 0.5 * acc;
}

double edge_len(const manifold::vec2 &a, const manifold::vec2 &b) {
  const double dx = b.x - a.x;
  const double dy = b.y - a.y;
  return std::sqrt(dx * dx + dy * dy);
}

bool classify_regular_polygon(const std::vector<manifold::vec2> &poly,
                              manifold::vec2 *center,
                              double *side_len) {
  if (poly.size() < 3) return false;
  manifold::vec2 c(0.0, 0.0);
  for (const manifold::vec2 &p : poly) c += p;
  c /= (double)poly.size();

  std::vector<double> edges;
  std::vector<double> radii;
  edges.reserve(poly.size());
  radii.reserve(poly.size());
  for (size_t i = 0; i < poly.size(); ++i) {
    edges.push_back(edge_len(poly[i], poly[(i + 1) % poly.size()]));
    radii.push_back(edge_len(poly[i], c));
  }
  const double edge_min = *std::min_element(edges.begin(), edges.end());
  const double edge_max = *std::max_element(edges.begin(), edges.end());
  const double rad_min = *std::min_element(radii.begin(), radii.end());
  const double rad_max = *std::max_element(radii.begin(), radii.end());
  if (edge_max <= 1e-9 || rad_max <= 1e-9) return false;
  if ((edge_max - edge_min) / edge_max > 0.025) return false;
  if ((rad_max - rad_min) / rad_max > 0.025) return false;

  if (center) *center = c;
  if (side_len) *side_len = edge_max;
  return true;
}

std::vector<manifold::vec2> rectangle_vertices(double w, double h, bool centered) {
  const double x0 = centered ? -w * 0.5 : 0.0;
  const double y0 = centered ? -h * 0.5 : 0.0;
  const double x1 = x0 + w;
  const double y1 = y0 + h;
  std::vector<manifold::vec2> out;
  out.reserve(4);
  out.push_back(manifold::vec2(x0, y0));
  out.push_back(manifold::vec2(x1, y0));
  out.push_back(manifold::vec2(x1, y1));
  out.push_back(manifold::vec2(x0, y1));
  return out;
}

struct EvalSketchNode {
  bool ok = false;
  bool fallback_only = false;
  std::vector<manifold::vec2> vertices;
  manifold::vec2 anchor = manifold::vec2(0.0, 0.0);
  SketchPrimitiveKind primitive = SketchPrimitiveKind::Unknown;
  bool has_rect_size = false;
  double rect_w = 0.0;
  double rect_h = 0.0;
  bool has_circle = false;
  double circle_r = 0.0;
  bool has_fillet = false;
  double fillet_radius = 0.0;
};

bool eval_sketch_node(const ReplayTables &tables,
                      uint32_t id,
                      EvalSketchNode *out,
                      std::unordered_set<uint32_t> *visiting,
                      std::string *error) {
  if ((size_t)id >= tables.node_semantics.size() || !tables.node_semantics[id].valid) {
    *error = "Replay failed: missing semantic node " + std::to_string(id);
    return false;
  }
  if (visiting->find(id) != visiting->end()) {
    *error = "Replay failed: cyclic semantic node graph.";
    return false;
  }

  const ReplayNodeSemantic &node = tables.node_semantics[id];
  visiting->insert(id);
  EvalSketchNode res;

  switch ((OpCode)node.opcode) {
    case OpCode::CrossRect:
    case OpCode::CrossSquare: {
      if (node.params_f64.size() < 2 || node.params_u32.empty()) {
        *error = "Replay failed: malformed rect semantic node.";
        visiting->erase(id);
        return false;
      }
      const double w = std::fabs(node.params_f64[0]);
      const double h = std::fabs(node.params_f64[1]);
      const bool centered = (node.params_u32[0] != 0);
      res.ok = true;
      res.primitive = SketchPrimitiveKind::Rect;
      res.vertices = rectangle_vertices(w, h, centered);
      res.has_rect_size = true;
      res.rect_w = w;
      res.rect_h = h;
      res.anchor = centered ? manifold::vec2(0.0, 0.0) : manifold::vec2(w * 0.5, h * 0.5);
    } break;
    case OpCode::CrossPolygons: {
      if (!node.has_polygons || node.polygons.empty()) {
        *error = "Replay failed: malformed cross polygon semantic node.";
        visiting->erase(id);
        return false;
      }
      const manifold::SimplePolygon *best = nullptr;
      double best_area = -1.0;
      for (const manifold::SimplePolygon &poly : node.polygons) {
        std::vector<manifold::vec2> tmp(poly.begin(), poly.end());
        const double a = std::fabs(polygon_area(tmp));
        if (a > best_area) {
          best_area = a;
          best = &poly;
        }
      }
      if (!best || best->size() < 3) {
        *error = "Replay failed: missing polygon shell for sketch dimensions.";
        visiting->erase(id);
        return false;
      }
      res.ok = true;
      res.vertices.assign(best->begin(), best->end());
      manifold::vec2 c(0.0, 0.0);
      for (const manifold::vec2 &p : res.vertices) c += p;
      c /= (double)res.vertices.size();
      res.anchor = c;
      manifold::vec2 rp_center;
      double rp_side = 0.0;
      if (classify_regular_polygon(res.vertices, &rp_center, &rp_side)) {
        res.primitive = SketchPrimitiveKind::RegularPolygon;
      } else {
        res.primitive = SketchPrimitiveKind::IrregularPolygon;
      }
    } break;
    case OpCode::CrossCircle: {
      if (node.params_f64.empty()) {
        *error = "Replay failed: malformed circle semantic node.";
        visiting->erase(id);
        return false;
      }
      res.ok = true;
      res.primitive = SketchPrimitiveKind::Circle;
      res.anchor = manifold::vec2(0.0, 0.0);
      res.has_circle = true;
      res.circle_r = std::fabs(node.params_f64[0]);
    } break;
    case OpCode::CrossPoint: {
      if (node.params_f64.size() < 3) {
        *error = "Replay failed: malformed point semantic node.";
        visiting->erase(id);
        return false;
      }
      res.ok = true;
      res.primitive = SketchPrimitiveKind::Point;
      res.anchor = manifold::vec2(node.params_f64[0], node.params_f64[1]);
      res.has_circle = true;
      res.circle_r = std::fabs(node.params_f64[2]);
    } break;
    case OpCode::CrossTranslate:
    case OpCode::CrossRotate:
    case OpCode::CrossFillet:
    case OpCode::CrossOffsetClone: {
      if (node.inputs.empty()) {
        *error = "Replay failed: malformed cross transform semantic node.";
        visiting->erase(id);
        return false;
      }
      EvalSketchNode base;
      if (!eval_sketch_node(tables, node.inputs[0], &base, visiting, error)) {
        visiting->erase(id);
        return false;
      }
      res = base;

      if ((OpCode)node.opcode == OpCode::CrossTranslate) {
        if (node.params_f64.size() < 2) {
          *error = "Replay failed: malformed cross translate semantic node.";
          visiting->erase(id);
          return false;
        }
        const Affine2 t = translation2(node.params_f64[0], node.params_f64[1]);
        for (manifold::vec2 &p : res.vertices) p = apply_affine(t, p);
        res.anchor = apply_affine(t, res.anchor);
      } else if ((OpCode)node.opcode == OpCode::CrossRotate) {
        if (node.params_f64.empty()) {
          *error = "Replay failed: malformed cross rotate semantic node.";
          visiting->erase(id);
          return false;
        }
        const Affine2 t = rotation2(node.params_f64[0]);
        for (manifold::vec2 &p : res.vertices) p = apply_affine(t, p);
        res.anchor = apply_affine(t, res.anchor);
      } else if ((OpCode)node.opcode == OpCode::CrossFillet) {
        if (node.params_f64.empty()) {
          *error = "Replay failed: malformed cross fillet semantic node.";
          visiting->erase(id);
          return false;
        }
        res.has_fillet = true;
        res.fillet_radius = std::fabs(node.params_f64[0]);
      } else {
        res.fallback_only = true;
      }
    } break;
    default:
      res.fallback_only = true;
      res.ok = true;
      break;
  }

  visiting->erase(id);
  *out = std::move(res);
  return true;
}

}  // namespace

bool BuildSketchDimensionModelForRoot(const ReplayTables &tables, uint32_t root_id,
                                      SketchDimensionModel *out, std::string *error) {
  if (!out) {
    if (error) *error = "Replay failed: null output model.";
    return false;
  }
  if ((size_t)root_id >= tables.cross_nodes.size() || !tables.has_cross[root_id]) {
    if (error) *error = "Replay failed: root cross-section node missing.";
    return false;
  }

  std::unordered_set<uint32_t> visiting;
  EvalSketchNode node;
  std::string local_err;
  if (!eval_sketch_node(tables, root_id, &node, &visiting, &local_err)) {
    if (error) *error = local_err;
    return false;
  }
  if (node.fallback_only) {
    if (error) *error = "Sketch semantic model requires contour fallback for this operation chain.";
    return false;
  }

  SketchDimensionModel model;
  model.primitive = node.primitive;
  model.logicalVertices = node.vertices;
  model.anchor = node.anchor;
  model.hasRectSize = node.has_rect_size;
  model.rectWidth = node.rect_w;
  model.rectHeight = node.rect_h;
  model.hasCircleRadius = node.has_circle;
  model.circleRadius = node.circle_r;
  model.hasFillet = node.has_fillet;
  model.filletRadius = node.fillet_radius;

  if (node.primitive == SketchPrimitiveKind::Rect && node.vertices.size() == 4) {
    SketchDimensionEntity w;
    w.kind = SketchDimensionEntity::Kind::LineDim;
    w.line.a = node.vertices[0];
    w.line.b = node.vertices[1];
    w.line.value = node.has_rect_size ? node.rect_w : edge_len(node.vertices[0], node.vertices[1]);
    model.entities.push_back(w);

    SketchDimensionEntity h;
    h.kind = SketchDimensionEntity::Kind::LineDim;
    h.line.a = node.vertices[1];
    h.line.b = node.vertices[2];
    h.line.value = node.has_rect_size ? node.rect_h : edge_len(node.vertices[1], node.vertices[2]);
    model.entities.push_back(h);
  } else if (node.primitive == SketchPrimitiveKind::IrregularPolygon ||
             node.primitive == SketchPrimitiveKind::RegularPolygon) {
    model.polygonSides = (uint32_t)node.vertices.size();
    model.regularPolygon = (node.primitive == SketchPrimitiveKind::RegularPolygon);
    for (size_t i = 0; i < node.vertices.size(); ++i) {
      SketchDimensionEntity e;
      e.kind = SketchDimensionEntity::Kind::LineDim;
      e.line.a = node.vertices[i];
      e.line.b = node.vertices[(i + 1) % node.vertices.size()];
      e.line.value = edge_len(e.line.a, e.line.b);
      model.entities.push_back(e);
    }
  }

  *out = std::move(model);
  return true;
}

}  // namespace vicad
