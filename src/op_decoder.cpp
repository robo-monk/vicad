#include "op_decoder.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#include "ipc_protocol.h"

namespace vicad {

namespace {

struct Reader {
  const uint8_t *ptr;
  size_t len;
  size_t off;
};

template <typename T>
bool read_pod(Reader *r, T *out) {
  if (r->off + sizeof(T) > r->len) return false;
  std::memcpy(out, r->ptr + r->off, sizeof(T));
  r->off += sizeof(T);
  return true;
}

bool read_u32(Reader *r, uint32_t *out) { return read_pod<uint32_t>(r, out); }
bool read_f64(Reader *r, double *out) { return read_pod<double>(r, out); }

bool ensure_node(std::vector<manifold::Manifold> *m_nodes,
                 std::vector<bool> *has_m,
                 std::vector<manifold::CrossSection> *c_nodes,
                 std::vector<bool> *has_c,
                 std::vector<ReplayNodeSemantic> *sem,
                 uint32_t id) {
  const size_t need = (size_t)id + 1;
  if (m_nodes->size() >= need) return true;
  m_nodes->resize(need);
  has_m->resize(need, false);
  c_nodes->resize(need);
  has_c->resize(need, false);
  sem->resize(need);
  return true;
}

bool need_m(const std::vector<manifold::Manifold> &nodes,
            const std::vector<bool> &has, uint32_t id, manifold::Manifold *out,
            std::string *error) {
  if ((size_t)id >= nodes.size() || !has[id]) {
    *error = "Replay failed: missing manifold node " + std::to_string(id);
    return false;
  }
  *out = nodes[id];
  return true;
}

bool need_c(const std::vector<manifold::CrossSection> &nodes,
            const std::vector<bool> &has, uint32_t id, manifold::CrossSection *out,
            std::string *error) {
  if ((size_t)id >= nodes.size() || !has[id]) {
    *error = "Replay failed: missing cross-section node " + std::to_string(id);
    return false;
  }
  *out = nodes[id];
  return true;
}

bool check_status(const manifold::Manifold &m, const char *ctx, std::string *error) {
  if (m.Status() == manifold::Manifold::Error::NoError) return true;
  *error = std::string("Replay failed in ") + ctx + ": status=" + std::to_string((int)m.Status());
  return false;
}

const char *op_name(uint16_t opcode) {
  switch ((OpCode)opcode) {
    case OpCode::Sphere: return "Sphere";
    case OpCode::Cube: return "Cube";
    case OpCode::Cylinder: return "Cylinder";
    case OpCode::Union: return "Union";
    case OpCode::Subtract: return "Subtract";
    case OpCode::Intersect: return "Intersect";
    case OpCode::Translate: return "Translate";
    case OpCode::Rotate: return "Rotate";
    case OpCode::Scale: return "Scale";
    case OpCode::Extrude: return "Extrude";
    case OpCode::Revolve: return "Revolve";
    case OpCode::Slice: return "Slice";
    case OpCode::CrossCircle: return "CrossCircle";
    case OpCode::CrossSquare: return "CrossSquare";
    case OpCode::CrossTranslate: return "CrossTranslate";
    case OpCode::CrossRotate: return "CrossRotate";
    case OpCode::CrossRect: return "CrossRect";
    case OpCode::CrossPoint: return "CrossPoint";
    case OpCode::CrossPolygons: return "CrossPolygons";
    case OpCode::CrossFillet: return "CrossFillet";
    case OpCode::CrossOffsetClone: return "CrossOffsetClone";
    default: return "Unknown";
  }
}

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
  for (const manifold::vec2 &p : poly) {
    c += p;
  }
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
        // Offset clone changes geometry in ways this semantic model does not fully reconstruct yet.
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

void collect_trace_postorder(const ReplayTables &tables,
                             uint32_t id,
                             std::unordered_set<uint32_t> *visited,
                             std::vector<uint32_t> *order) {
  if ((size_t)id >= tables.node_semantics.size()) return;
  const ReplayNodeSemantic &node = tables.node_semantics[id];
  if (!node.valid) return;
  if (!visited->insert(id).second) return;
  for (uint32_t in_id : node.inputs) {
    collect_trace_postorder(tables, in_id, visited, order);
  }
  order->push_back(id);
}

}  // namespace

bool ReplayOpsToTables(const uint8_t *records, size_t records_size, uint32_t op_count,
                       ReplayTables *tables, std::string *error) {
  tables->manifold_nodes.clear();
  tables->has_manifold.clear();
  tables->cross_nodes.clear();
  tables->has_cross.clear();
  tables->node_semantics.clear();

  std::vector<manifold::Manifold> &m_nodes = tables->manifold_nodes;
  std::vector<bool> &has_m = tables->has_manifold;
  std::vector<manifold::CrossSection> &c_nodes = tables->cross_nodes;
  std::vector<bool> &has_c = tables->has_cross;
  std::vector<ReplayNodeSemantic> &semantics = tables->node_semantics;

  Reader ops = {records, records_size, 0};
  uint32_t parsed = 0;
  while (ops.off < ops.len) {
    OpRecordHeader hdr = {};
    if (!read_pod(&ops, &hdr)) {
      *error = "Replay failed: truncated op header.";
      return false;
    }
    if (ops.off + hdr.payload_len > ops.len) {
      *error = "Replay failed: truncated op payload.";
      return false;
    }

    Reader payload = {ops.ptr + ops.off, hdr.payload_len, 0};
    ops.off += hdr.payload_len;
    parsed++;

    uint32_t out_id = 0;
    if (!read_u32(&payload, &out_id)) {
      *error = "Replay failed: missing out node id.";
      return false;
    }
    ensure_node(&m_nodes, &has_m, &c_nodes, &has_c, &semantics, out_id);

    ReplayNodeSemantic sem = {};
    sem.opcode = hdr.opcode;
    sem.out_id = out_id;

    switch ((OpCode)hdr.opcode) {
      case OpCode::Sphere: {
        double radius = 0.0;
        uint32_t seg = 0;
        if (!read_f64(&payload, &radius) || !read_u32(&payload, &seg)) {
          *error = "Replay failed: invalid sphere payload.";
          return false;
        }
        manifold::Manifold m = manifold::Manifold::Sphere(radius, (int)seg);
        if (!check_status(m, "sphere", error)) return false;
        m_nodes[out_id] = std::move(m);
        has_m[out_id] = true;
        sem.params_f64.push_back(radius);
        sem.params_u32.push_back(seg);
      } break;
      case OpCode::Cube: {
        double x = 0.0, y = 0.0, z = 0.0;
        uint32_t center = 0;
        if (!read_f64(&payload, &x) || !read_f64(&payload, &y) || !read_f64(&payload, &z) ||
            !read_u32(&payload, &center)) {
          *error = "Replay failed: invalid cube payload.";
          return false;
        }
        manifold::Manifold m = manifold::Manifold::Cube(manifold::vec3(x, y, z), center != 0);
        if (!check_status(m, "cube", error)) return false;
        m_nodes[out_id] = std::move(m);
        has_m[out_id] = true;
        sem.params_f64 = {x, y, z};
        sem.params_u32 = {center};
      } break;
      case OpCode::Cylinder: {
        double h = 0.0, r1 = 0.0, r2 = 0.0;
        uint32_t seg = 0, center = 0;
        if (!read_f64(&payload, &h) || !read_f64(&payload, &r1) || !read_f64(&payload, &r2) ||
            !read_u32(&payload, &seg) || !read_u32(&payload, &center)) {
          *error = "Replay failed: invalid cylinder payload.";
          return false;
        }
        manifold::Manifold m = manifold::Manifold::Cylinder(h, r1, r2, (int)seg, center != 0);
        if (!check_status(m, "cylinder", error)) return false;
        m_nodes[out_id] = std::move(m);
        has_m[out_id] = true;
        sem.params_f64 = {h, r1, r2};
        sem.params_u32 = {seg, center};
      } break;
      case OpCode::Union: {
        uint32_t count = 0;
        if (!read_u32(&payload, &count) || count == 0) {
          *error = "Replay failed: invalid union payload.";
          return false;
        }
        std::vector<manifold::Manifold> parts;
        parts.reserve(count);
        sem.params_u32.push_back(count);
        for (uint32_t i = 0; i < count; ++i) {
          uint32_t id = 0;
          if (!read_u32(&payload, &id)) {
            *error = "Replay failed: invalid union args.";
            return false;
          }
          manifold::Manifold part;
          if (!need_m(m_nodes, has_m, id, &part, error)) return false;
          parts.push_back(part);
          sem.inputs.push_back(id);
        }
        manifold::Manifold m = manifold::Manifold::BatchBoolean(parts, manifold::OpType::Add);
        if (!check_status(m, "union", error)) return false;
        m_nodes[out_id] = std::move(m);
        has_m[out_id] = true;
      } break;
      case OpCode::Subtract:
      case OpCode::Intersect: {
        uint32_t a = 0, b = 0;
        if (!read_u32(&payload, &a) || !read_u32(&payload, &b)) {
          *error = "Replay failed: invalid boolean payload.";
          return false;
        }
        manifold::Manifold ma, mb;
        if (!need_m(m_nodes, has_m, a, &ma, error) || !need_m(m_nodes, has_m, b, &mb, error)) {
          return false;
        }
        const manifold::OpType op =
            ((OpCode)hdr.opcode == OpCode::Subtract) ? manifold::OpType::Subtract : manifold::OpType::Intersect;
        manifold::Manifold m = ma.Boolean(mb, op);
        if (!check_status(m, "boolean", error)) return false;
        m_nodes[out_id] = std::move(m);
        has_m[out_id] = true;
        sem.inputs = {a, b};
      } break;
      case OpCode::Translate:
      case OpCode::Rotate:
      case OpCode::Scale: {
        uint32_t in_id = 0;
        double x = 0.0, y = 0.0, z = 0.0;
        if (!read_u32(&payload, &in_id) || !read_f64(&payload, &x) || !read_f64(&payload, &y) ||
            !read_f64(&payload, &z)) {
          *error = "Replay failed: invalid transform payload.";
          return false;
        }
        manifold::Manifold in_m;
        if (!need_m(m_nodes, has_m, in_id, &in_m, error)) return false;
        manifold::Manifold out_m;
        if ((OpCode)hdr.opcode == OpCode::Translate) {
          out_m = in_m.Translate(manifold::vec3(x, y, z));
        } else if ((OpCode)hdr.opcode == OpCode::Rotate) {
          out_m = in_m.Rotate(x, y, z);
        } else {
          out_m = in_m.Scale(manifold::vec3(x, y, z));
        }
        if (!check_status(out_m, "transform", error)) return false;
        m_nodes[out_id] = std::move(out_m);
        has_m[out_id] = true;
        sem.inputs = {in_id};
        sem.params_f64 = {x, y, z};
      } break;
      case OpCode::CrossCircle: {
        double radius = 0.0;
        uint32_t seg = 0;
        if (!read_f64(&payload, &radius) || !read_u32(&payload, &seg)) {
          *error = "Replay failed: invalid cross circle payload.";
          return false;
        }
        c_nodes[out_id] = manifold::CrossSection::Circle(radius, (int)seg);
        has_c[out_id] = true;
        sem.params_f64 = {radius};
        sem.params_u32 = {seg};
      } break;
      case OpCode::CrossSquare: {
        double x = 0.0, y = 0.0;
        uint32_t center = 0;
        if (!read_f64(&payload, &x) || !read_f64(&payload, &y) || !read_u32(&payload, &center)) {
          *error = "Replay failed: invalid cross square payload.";
          return false;
        }
        c_nodes[out_id] = manifold::CrossSection::Square(manifold::vec2(x, y), center != 0);
        has_c[out_id] = true;
        sem.params_f64 = {x, y};
        sem.params_u32 = {center};
      } break;
      case OpCode::CrossRect: {
        double x = 0.0, y = 0.0;
        uint32_t center = 0;
        if (!read_f64(&payload, &x) || !read_f64(&payload, &y) || !read_u32(&payload, &center)) {
          *error = "Replay failed: invalid cross rect payload.";
          return false;
        }
        c_nodes[out_id] = manifold::CrossSection::Square(manifold::vec2(x, y), center != 0);
        has_c[out_id] = true;
        sem.params_f64 = {x, y};
        sem.params_u32 = {center};
      } break;
      case OpCode::CrossPoint: {
        double x = 0.0, y = 0.0, radius = 0.0;
        uint32_t seg = 0;
        if (!read_f64(&payload, &x) || !read_f64(&payload, &y) || !read_f64(&payload, &radius) ||
            !read_u32(&payload, &seg)) {
          *error = "Replay failed: invalid cross point payload.";
          return false;
        }
        c_nodes[out_id] =
            manifold::CrossSection::Circle(radius, (int)seg).Translate(manifold::vec2(x, y));
        has_c[out_id] = true;
        sem.params_f64 = {x, y, radius};
        sem.params_u32 = {seg};
      } break;
      case OpCode::CrossPolygons: {
        uint32_t contour_count = 0;
        if (!read_u32(&payload, &contour_count) || contour_count == 0) {
          *error = "Replay failed: invalid cross polygons payload.";
          return false;
        }
        manifold::Polygons polys;
        polys.reserve(contour_count);
        for (uint32_t c = 0; c < contour_count; ++c) {
          uint32_t point_count = 0;
          if (!read_u32(&payload, &point_count) || point_count < 3) {
            *error = "Replay failed: invalid cross polygon contour payload.";
            return false;
          }
          manifold::SimplePolygon poly;
          poly.reserve(point_count);
          for (uint32_t i = 0; i < point_count; ++i) {
            double x = 0.0, y = 0.0;
            if (!read_f64(&payload, &x) || !read_f64(&payload, &y)) {
              *error = "Replay failed: invalid cross polygon point payload.";
              return false;
            }
            poly.push_back(manifold::vec2(x, y));
          }
          polys.push_back(std::move(poly));
        }
        c_nodes[out_id] = manifold::CrossSection(polys, manifold::CrossSection::FillRule::Positive);
        has_c[out_id] = true;
        sem.has_polygons = true;
        sem.polygons = polys;
      } break;
      case OpCode::CrossTranslate: {
        uint32_t in_id = 0;
        double x = 0.0, y = 0.0;
        if (!read_u32(&payload, &in_id) || !read_f64(&payload, &x) || !read_f64(&payload, &y)) {
          *error = "Replay failed: invalid cross translate payload.";
          return false;
        }
        manifold::CrossSection in_c;
        if (!need_c(c_nodes, has_c, in_id, &in_c, error)) return false;
        c_nodes[out_id] = in_c.Translate(manifold::vec2(x, y));
        has_c[out_id] = true;
        sem.inputs = {in_id};
        sem.params_f64 = {x, y};
      } break;
      case OpCode::CrossRotate: {
        uint32_t in_id = 0;
        double deg = 0.0;
        if (!read_u32(&payload, &in_id) || !read_f64(&payload, &deg)) {
          *error = "Replay failed: invalid cross rotate payload.";
          return false;
        }
        manifold::CrossSection in_c;
        if (!need_c(c_nodes, has_c, in_id, &in_c, error)) return false;
        c_nodes[out_id] = in_c.Rotate(deg);
        has_c[out_id] = true;
        sem.inputs = {in_id};
        sem.params_f64 = {deg};
      } break;
      case OpCode::CrossFillet: {
        uint32_t in_id = 0;
        double radius = 0.0;
        if (!read_u32(&payload, &in_id) || !read_f64(&payload, &radius)) {
          *error = "Replay failed: invalid cross fillet payload.";
          return false;
        }
        if (radius < 0.0) {
          *error = "Replay failed: cross fillet radius must be >= 0.";
          return false;
        }
        manifold::CrossSection in_c;
        if (!need_c(c_nodes, has_c, in_id, &in_c, error)) return false;
        if (radius == 0.0) {
          c_nodes[out_id] = in_c;
          has_c[out_id] = true;
        } else {
          manifold::CrossSection inset = in_c.Offset(-radius, manifold::CrossSection::JoinType::Miter);
          if (inset.IsEmpty()) {
            *error = "Replay failed: fillet radius is too large for this cross-section.";
            return false;
          }
          manifold::CrossSection rounded = inset.Offset(radius, manifold::CrossSection::JoinType::Round);
          if (rounded.IsEmpty()) {
            *error = "Replay failed: fillet operation produced an empty cross-section.";
            return false;
          }
          c_nodes[out_id] = std::move(rounded);
          has_c[out_id] = true;
        }
        sem.inputs = {in_id};
        sem.params_f64 = {radius};
      } break;
      case OpCode::CrossOffsetClone: {
        uint32_t in_id = 0;
        double delta = 0.0;
        if (!read_u32(&payload, &in_id) || !read_f64(&payload, &delta)) {
          *error = "Replay failed: invalid cross offset clone payload.";
          return false;
        }
        manifold::CrossSection in_c;
        if (!need_c(c_nodes, has_c, in_id, &in_c, error)) return false;
        manifold::CrossSection out_c = in_c.Offset(delta, manifold::CrossSection::JoinType::Miter);
        if (out_c.IsEmpty()) {
          *error = "Replay failed: offsetClone produced an empty cross-section.";
          return false;
        }
        c_nodes[out_id] = std::move(out_c);
        has_c[out_id] = true;
        sem.inputs = {in_id};
        sem.params_f64 = {delta};
      } break;
      case OpCode::Extrude: {
        uint32_t cs_id = 0;
        double h = 0.0, twist = 0.0;
        uint32_t div = 0;
        if (!read_u32(&payload, &cs_id) || !read_f64(&payload, &h) || !read_u32(&payload, &div) ||
            !read_f64(&payload, &twist)) {
          *error = "Replay failed: invalid extrude payload.";
          return false;
        }
        manifold::CrossSection cs;
        if (!need_c(c_nodes, has_c, cs_id, &cs, error)) return false;
        manifold::Manifold m = manifold::Manifold::Extrude(cs.ToPolygons(), h, (int)div, twist);
        if (!check_status(m, "extrude", error)) return false;
        m_nodes[out_id] = std::move(m);
        has_m[out_id] = true;
        sem.inputs = {cs_id};
        sem.params_f64 = {h, twist};
        sem.params_u32 = {div};
      } break;
      case OpCode::Revolve: {
        uint32_t cs_id = 0, seg = 0;
        double deg = 0.0;
        if (!read_u32(&payload, &cs_id) || !read_u32(&payload, &seg) || !read_f64(&payload, &deg)) {
          *error = "Replay failed: invalid revolve payload.";
          return false;
        }
        manifold::CrossSection cs;
        if (!need_c(c_nodes, has_c, cs_id, &cs, error)) return false;
        manifold::Manifold m = manifold::Manifold::Revolve(cs.ToPolygons(), (int)seg, deg);
        if (!check_status(m, "revolve", error)) return false;
        m_nodes[out_id] = std::move(m);
        has_m[out_id] = true;
        sem.inputs = {cs_id};
        sem.params_u32 = {seg};
        sem.params_f64 = {deg};
      } break;
      case OpCode::Slice: {
        uint32_t in_id = 0;
        double z = 0.0;
        if (!read_u32(&payload, &in_id) || !read_f64(&payload, &z)) {
          *error = "Replay failed: invalid slice payload.";
          return false;
        }
        manifold::Manifold in_m;
        if (!need_m(m_nodes, has_m, in_id, &in_m, error)) return false;
        c_nodes[out_id] =
            manifold::CrossSection(in_m.Slice(z), manifold::CrossSection::FillRule::Positive);
        has_c[out_id] = true;
        sem.inputs = {in_id};
        sem.params_f64 = {z};
      } break;
      default:
        *error = "Replay failed: unknown opcode " + std::to_string(hdr.opcode);
        return false;
    }

    if (payload.off != payload.len) {
      *error = "Replay failed: payload trailing bytes for opcode " + std::to_string(hdr.opcode);
      return false;
    }

    sem.valid = true;
    semantics[out_id] = std::move(sem);
  }

  if (parsed != op_count) {
    *error = "Replay failed: op count mismatch.";
    return false;
  }
  return true;
}

bool ResolveReplayManifold(const ReplayTables &tables, uint32_t root_kind, uint32_t root_id,
                           manifold::Manifold *out, std::string *error) {
  if (root_kind != (uint32_t)NodeKind::Manifold) {
    *error = "Replay failed: root node is not a manifold.";
    return false;
  }
  if ((size_t)root_id >= tables.manifold_nodes.size() || !tables.has_manifold[root_id]) {
    *error = "Replay failed: root manifold node missing.";
    return false;
  }
  manifold::Manifold m = tables.manifold_nodes[root_id];
  if (!check_status(m, "final", error)) return false;
  *out = std::move(m);
  return true;
}

bool ResolveReplayCrossSection(const ReplayTables &tables, uint32_t root_kind, uint32_t root_id,
                               manifold::CrossSection *out, std::string *error) {
  if (root_kind != (uint32_t)NodeKind::CrossSection) {
    *error = "Replay failed: root node is not a cross-section.";
    return false;
  }
  if ((size_t)root_id >= tables.cross_nodes.size() || !tables.has_cross[root_id]) {
    *error = "Replay failed: root cross-section node missing.";
    return false;
  }
  *out = tables.cross_nodes[root_id];
  return true;
}

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

bool BuildOperationTraceForRoot(const ReplayTables &tables, uint32_t root_kind, uint32_t root_id,
                                std::vector<OpTraceEntry> *out, std::string *error) {
  if (!out) {
    if (error) *error = "Replay failed: null operation trace output.";
    return false;
  }

  if (root_kind == (uint32_t)NodeKind::Manifold) {
    if ((size_t)root_id >= tables.has_manifold.size() || !tables.has_manifold[root_id]) {
      if (error) *error = "Replay failed: root manifold node missing.";
      return false;
    }
  } else if (root_kind == (uint32_t)NodeKind::CrossSection) {
    if ((size_t)root_id >= tables.has_cross.size() || !tables.has_cross[root_id]) {
      if (error) *error = "Replay failed: root cross-section node missing.";
      return false;
    }
  } else {
    if (error) *error = "Replay failed: unsupported root kind for operation trace.";
    return false;
  }

  std::unordered_set<uint32_t> visited;
  std::vector<uint32_t> order;
  collect_trace_postorder(tables, root_id, &visited, &order);

  out->clear();
  out->reserve(order.size());
  for (uint32_t id : order) {
    if ((size_t)id >= tables.node_semantics.size()) continue;
    const ReplayNodeSemantic &node = tables.node_semantics[id];
    if (!node.valid) continue;
    OpTraceEntry entry;
    entry.opcode = node.opcode;
    entry.name = op_name(node.opcode);
    entry.outId = node.out_id;
    entry.args.reserve(node.params_f64.size() + node.params_u32.size());
    for (double v : node.params_f64) entry.args.push_back(v);
    for (uint32_t v : node.params_u32) entry.args.push_back((double)v);
    out->push_back(std::move(entry));
  }

  return true;
}

bool ReplayOpsToMesh(const ReplayInput &in, manifold::MeshGL *mesh, std::string *error) {
  ReplayTables tables;
  if (!ReplayOpsToTables(in.records, in.records_size, in.op_count, &tables, error)) return false;
  manifold::Manifold out;
  if (!ResolveReplayManifold(tables, in.root_kind, in.root_id, &out, error)) return false;
  *mesh = out.GetMeshGL();
  return true;
}

}  // namespace vicad
