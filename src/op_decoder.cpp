#include "op_decoder.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
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
                 std::vector<SketchPlane> *cross_plane,
                 std::vector<ReplayNodeSemantic> *sem,
                 uint32_t id) {
  const size_t need = (size_t)id + 1;
  if (m_nodes->size() >= need) return true;
  m_nodes->resize(need);
  has_m->resize(need, false);
  c_nodes->resize(need);
  has_c->resize(need, false);
  cross_plane->resize(need);
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

double revolve_effective_radius(const manifold::Polygons &cross_section) {
  double radius = 0.0;
  for (const manifold::SimplePolygon &poly : cross_section) {
    size_t i = 0;
    while (i < poly.size() && poly[i].x < 0.0) {
      ++i;
    }
    if (i == poly.size()) continue;
    const size_t start = i;
    do {
      if (poly[i].x >= 0.0) {
        radius = std::max(radius, poly[i].x);
      }
      const size_t next = i + 1 == poly.size() ? 0 : i + 1;
      i = next;
    } while (i != start);
  }
  return radius;
}

bool valid_plane_kind(uint32_t kind) {
  return kind <= (uint32_t)SketchPlaneKind::YZ;
}

SketchPlane default_sketch_plane() {
  return SketchPlane{};
}

manifold::vec3 map_plane_local_to_world(const SketchPlane &plane, const manifold::vec3 &p) {
  switch (plane.kind) {
    case SketchPlaneKind::XY:
      return manifold::vec3(p.x, p.y, p.z + plane.offset);
    case SketchPlaneKind::XZ:
      // Keep right-handed basis while preserving +extrude along +Y.
      return manifold::vec3(p.x, p.z + plane.offset, -p.y);
    case SketchPlaneKind::YZ:
      return manifold::vec3(p.z + plane.offset, p.x, p.y);
  }
  return manifold::vec3(p.x, p.y, p.z + plane.offset);
}

manifold::Manifold apply_plane_to_manifold(const manifold::Manifold &in_m, const SketchPlane &plane) {
  if (plane.kind == SketchPlaneKind::XY && std::abs(plane.offset) <= 1e-12) {
    return in_m;
  }
  return in_m.Warp([plane](manifold::vec3 &v) {
    v = map_plane_local_to_world(plane, v);
  });
}

double cross2(const manifold::vec2 &a, const manifold::vec2 &b) {
  return a.x * b.y - a.y * b.x;
}

double poly_area(const manifold::SimplePolygon &poly) {
  if (poly.size() < 3) return 0.0;
  double acc = 0.0;
  for (size_t i = 0; i < poly.size(); ++i) {
    const manifold::vec2 &a = poly[i];
    const manifold::vec2 &b = poly[(i + 1) % poly.size()];
    acc += a.x * b.y - b.x * a.y;
  }
  return 0.5 * acc;
}

double vec_len(const manifold::vec2 &v) {
  return std::sqrt(v.x * v.x + v.y * v.y);
}

manifold::vec2 vec_norm(const manifold::vec2 &v) {
  const double l = vec_len(v);
  if (l <= 1e-15) return manifold::vec2(0.0, 0.0);
  return manifold::vec2(v.x / l, v.y / l);
}

bool nearly_same_point(const manifold::vec2 &a, const manifold::vec2 &b, double eps) {
  return std::abs(a.x - b.x) <= eps && std::abs(a.y - b.y) <= eps;
}

struct CornerFilletSpec {
  uint32_t contour = 0;
  uint32_t vertex = 0;
  double radius = 0.0;
};

bool apply_corner_fillets(const manifold::CrossSection &in_c,
                          const std::vector<CornerFilletSpec> &specs,
                          const ReplayLodPolicy &lod_policy,
                          manifold::CrossSection *out_c,
                          std::string *error) {
  if (!out_c) {
    if (error) *error = "Replay failed: null cross-section output for corner fillet.";
    return false;
  }
  if (specs.empty()) {
    *out_c = in_c;
    return true;
  }

  manifold::Polygons polys = in_c.ToPolygons();
  if (polys.empty()) {
    if (error) *error = "Replay failed: cross fillet corners requires a non-empty cross-section.";
    return false;
  }

  std::unordered_map<uint32_t, std::unordered_map<uint32_t, double>> corner_radii;
  corner_radii.reserve(specs.size());
  for (const CornerFilletSpec &spec : specs) {
    if (!std::isfinite(spec.radius) || spec.radius < 0.0) {
      if (error) *error = "Replay failed: cross fillet corner radius must be finite and >= 0.";
      return false;
    }
    if ((size_t)spec.contour >= polys.size()) {
      if (error) *error = "Replay failed: cross fillet corner contour index out of range.";
      return false;
    }
    const manifold::SimplePolygon &poly = polys[spec.contour];
    if (poly.size() < 3) {
      if (error) *error = "Replay failed: cross fillet corner contour has fewer than 3 vertices.";
      return false;
    }
    if ((size_t)spec.vertex >= poly.size()) {
      if (error) *error = "Replay failed: cross fillet corner vertex index out of range.";
      return false;
    }
    auto &by_vertex = corner_radii[spec.contour];
    if (by_vertex.find(spec.vertex) != by_vertex.end()) {
      if (error) *error = "Replay failed: duplicate cross fillet corner selection.";
      return false;
    }
    by_vertex[spec.vertex] = spec.radius;
  }

  manifold::Polygons out_polys;
  out_polys.reserve(polys.size());
  constexpr double kGeomEps = 1e-9;
  constexpr double kPi = 3.14159265358979323846;
  constexpr double kTwoPi = 2.0 * kPi;

  for (size_t ci = 0; ci < polys.size(); ++ci) {
    const manifold::SimplePolygon &poly = polys[ci];
    if (poly.size() < 3) {
      if (error) *error = "Replay failed: cross fillet corner contour has fewer than 3 vertices.";
      return false;
    }

    const double area = poly_area(poly);
    if (std::abs(area) <= kGeomEps) {
      if (error) *error = "Replay failed: cross fillet corners cannot be applied to degenerate contours.";
      return false;
    }
    const double contour_dir = (area > 0.0) ? 1.0 : -1.0;

    std::vector<double> radii(poly.size(), 0.0);
    auto it_contour = corner_radii.find((uint32_t)ci);
    if (it_contour != corner_radii.end()) {
      for (const auto &entry : it_contour->second) {
        radii[entry.first] = entry.second;
      }
    }

    std::vector<double> tangent_dist(poly.size(), 0.0);
    for (size_t i = 0; i < poly.size(); ++i) {
      const double r = radii[i];
      if (r <= kGeomEps) continue;
      const size_t ip = (i + poly.size() - 1) % poly.size();
      const size_t in = (i + 1) % poly.size();
      const manifold::vec2 &prev = poly[ip];
      const manifold::vec2 &curr = poly[i];
      const manifold::vec2 &next = poly[in];

      const manifold::vec2 e_in(curr.x - prev.x, curr.y - prev.y);
      const manifold::vec2 e_out(next.x - curr.x, next.y - curr.y);
      const double len_in = vec_len(e_in);
      const double len_out = vec_len(e_out);
      if (len_in <= kGeomEps || len_out <= kGeomEps) {
        if (error) *error = "Replay failed: cross fillet corners encountered a zero-length edge.";
        return false;
      }

      const double turn = cross2(e_in, e_out);
      if (turn * contour_dir <= kGeomEps) {
        if (error) *error = "Replay failed: cross fillet corners only supports convex vertices.";
        return false;
      }

      const manifold::vec2 u_prev = vec_norm(manifold::vec2(prev.x - curr.x, prev.y - curr.y));
      const manifold::vec2 u_next = vec_norm(manifold::vec2(next.x - curr.x, next.y - curr.y));
      const double dot = std::clamp(u_prev.x * u_next.x + u_prev.y * u_next.y, -1.0, 1.0);
      const double alpha = std::acos(dot);
      if (!(alpha > 1e-6 && alpha < kPi - 1e-6)) {
        if (error) *error = "Replay failed: cross fillet corner angle is invalid.";
        return false;
      }
      const double tan_half = std::tan(alpha * 0.5);
      if (tan_half <= kGeomEps) {
        if (error) *error = "Replay failed: cross fillet corner angle is too sharp.";
        return false;
      }
      const double t = r / tan_half;
      if (t >= len_in - kGeomEps || t >= len_out - kGeomEps) {
        if (error) *error = "Replay failed: cross fillet corner radius is too large for adjacent edges.";
        return false;
      }
      tangent_dist[i] = t;
    }

    for (size_t i = 0; i < poly.size(); ++i) {
      const size_t in = (i + 1) % poly.size();
      const manifold::vec2 edge(poly[in].x - poly[i].x, poly[in].y - poly[i].y);
      const double len_edge = vec_len(edge);
      if (len_edge <= kGeomEps) {
        if (error) *error = "Replay failed: cross fillet corners encountered a zero-length edge.";
        return false;
      }
      if (tangent_dist[i] + tangent_dist[in] >= len_edge - kGeomEps) {
        if (error) *error = "Replay failed: cross fillet corner radii overlap on an edge.";
        return false;
      }
    }

    manifold::SimplePolygon out_poly;
    out_poly.reserve(poly.size() * 3);
    auto push_point = [&out_poly](const manifold::vec2 &p) {
      constexpr double kPushEps = 1e-10;
      if (out_poly.empty() || !nearly_same_point(out_poly.back(), p, kPushEps)) {
        out_poly.push_back(p);
      }
    };

    for (size_t i = 0; i < poly.size(); ++i) {
      const double r = radii[i];
      if (r <= kGeomEps) {
        push_point(poly[i]);
        continue;
      }

      const size_t ip = (i + poly.size() - 1) % poly.size();
      const size_t in = (i + 1) % poly.size();
      const manifold::vec2 &prev = poly[ip];
      const manifold::vec2 &curr = poly[i];
      const manifold::vec2 &next = poly[in];
      const manifold::vec2 u_prev = vec_norm(manifold::vec2(prev.x - curr.x, prev.y - curr.y));
      const manifold::vec2 u_next = vec_norm(manifold::vec2(next.x - curr.x, next.y - curr.y));
      const double dot = std::clamp(u_prev.x * u_next.x + u_prev.y * u_next.y, -1.0, 1.0);
      const double alpha = std::acos(dot);
      const double dist_center = r / std::sin(alpha * 0.5);
      const manifold::vec2 bisector = vec_norm(manifold::vec2(u_prev.x + u_next.x, u_prev.y + u_next.y));
      if (vec_len(bisector) <= kGeomEps) {
        if (error) *error = "Replay failed: cross fillet corner bisector is undefined.";
        return false;
      }

      const double t = tangent_dist[i];
      const manifold::vec2 p_start(curr.x + u_prev.x * t, curr.y + u_prev.y * t);
      const manifold::vec2 p_end(curr.x + u_next.x * t, curr.y + u_next.y * t);
      const manifold::vec2 center(curr.x + bisector.x * dist_center,
                                  curr.y + bisector.y * dist_center);

      double a0 = std::atan2(p_start.y - center.y, p_start.x - center.x);
      double a1 = std::atan2(p_end.y - center.y, p_end.x - center.x);
      double delta = a1 - a0;
      if (contour_dir > 0.0) {
        while (delta <= 0.0) delta += kTwoPi;
      } else {
        while (delta >= 0.0) delta -= kTwoPi;
      }
      const double sweep = std::abs(delta);
      if (sweep <= 1e-8 || sweep >= kPi + 1e-6) {
        if (error) *error = "Replay failed: cross fillet corner arc sweep is invalid.";
        return false;
      }

      const int full_segments = AutoCircularSegments(std::abs(r), lod_policy.profile);
      int seg = std::max(1, (int)std::ceil((double)full_segments * sweep / kTwoPi));
      push_point(p_start);
      for (int s = 1; s < seg; ++s) {
        const double u = (double)s / (double)seg;
        const double a = a0 + delta * u;
        push_point(manifold::vec2(center.x + std::cos(a) * r,
                                  center.y + std::sin(a) * r));
      }
      push_point(p_end);
    }

    if (out_poly.size() >= 2 && nearly_same_point(out_poly.front(), out_poly.back(), 1e-10)) {
      out_poly.pop_back();
    }
    if (out_poly.size() < 3) {
      if (error) *error = "Replay failed: cross fillet corners produced an invalid contour.";
      return false;
    }
    out_polys.push_back(std::move(out_poly));
  }

  manifold::CrossSection out(out_polys, manifold::CrossSection::FillRule::Positive);
  if (out.IsEmpty()) {
    if (error) *error = "Replay failed: cross fillet corners produced an empty cross-section.";
    return false;
  }
  *out_c = std::move(out);
  return true;
}

// Sketch semantic derivation and operation trace construction are implemented in
// sketch_semantics.cpp and op_trace.cpp.

}  // namespace

bool ReplayOpsToTables(const uint8_t *records, size_t records_size, uint32_t op_count,
                       const ReplayLodPolicy &lod_policy,
                       ReplayTables *tables, std::string *error) {
  tables->manifold_nodes.clear();
  tables->has_manifold.clear();
  tables->cross_nodes.clear();
  tables->has_cross.clear();
  tables->cross_plane.clear();
  tables->node_semantics.clear();

  std::vector<manifold::Manifold> &m_nodes = tables->manifold_nodes;
  std::vector<bool> &has_m = tables->has_manifold;
  std::vector<manifold::CrossSection> &c_nodes = tables->cross_nodes;
  std::vector<bool> &has_c = tables->has_cross;
  std::vector<SketchPlane> &cross_plane = tables->cross_plane;
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
    ensure_node(&m_nodes, &has_m, &c_nodes, &has_c, &cross_plane, &semantics, out_id);

    ReplayNodeSemantic sem = {};
    sem.opcode = hdr.opcode;
    sem.out_id = out_id;

    switch ((OpCode)hdr.opcode) {
      case OpCode::Sphere: {
        double radius = 0.0;
        uint32_t ignored_segments = 0;
        if (!read_f64(&payload, &radius) || !read_u32(&payload, &ignored_segments)) {
          *error = "Replay failed: invalid sphere payload.";
          return false;
        }
        const uint32_t seg = (uint32_t)AutoCircularSegments(radius, lod_policy.profile);
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
        uint32_t ignored_segments = 0, center = 0;
        if (!read_f64(&payload, &h) || !read_f64(&payload, &r1) || !read_f64(&payload, &r2) ||
            !read_u32(&payload, &ignored_segments) || !read_u32(&payload, &center)) {
          *error = "Replay failed: invalid cylinder payload.";
          return false;
        }
        const double radius = std::max(std::abs(r1), std::abs(r2));
        const uint32_t seg = (uint32_t)AutoCircularSegments(radius, lod_policy.profile);
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
        uint32_t ignored_segments = 0;
        if (!read_f64(&payload, &radius) || !read_u32(&payload, &ignored_segments)) {
          *error = "Replay failed: invalid cross circle payload.";
          return false;
        }
        const uint32_t seg = (uint32_t)AutoCircularSegments(radius, lod_policy.profile);
        c_nodes[out_id] = manifold::CrossSection::Circle(radius, (int)seg);
        has_c[out_id] = true;
        cross_plane[out_id] = default_sketch_plane();
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
        cross_plane[out_id] = default_sketch_plane();
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
        cross_plane[out_id] = default_sketch_plane();
        sem.params_f64 = {x, y};
        sem.params_u32 = {center};
      } break;
      case OpCode::CrossPoint: {
        double x = 0.0, y = 0.0, radius = 0.0;
        uint32_t ignored_segments = 0;
        if (!read_f64(&payload, &x) || !read_f64(&payload, &y) || !read_f64(&payload, &radius) ||
            !read_u32(&payload, &ignored_segments)) {
          *error = "Replay failed: invalid cross point payload.";
          return false;
        }
        const uint32_t seg = (uint32_t)AutoCircularSegments(radius, lod_policy.profile);
        c_nodes[out_id] =
            manifold::CrossSection::Circle(radius, (int)seg).Translate(manifold::vec2(x, y));
        has_c[out_id] = true;
        cross_plane[out_id] = default_sketch_plane();
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
        cross_plane[out_id] = default_sketch_plane();
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
        cross_plane[out_id] = ((size_t)in_id < cross_plane.size()) ? cross_plane[in_id] : default_sketch_plane();
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
        cross_plane[out_id] = ((size_t)in_id < cross_plane.size()) ? cross_plane[in_id] : default_sketch_plane();
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
          const int fillet_segments =
              AutoCircularSegments(std::abs(radius), lod_policy.profile);
          manifold::CrossSection rounded =
              inset.Offset(radius, manifold::CrossSection::JoinType::Round,
                           2.0, fillet_segments);
          if (rounded.IsEmpty()) {
            *error = "Replay failed: fillet operation produced an empty cross-section.";
            return false;
          }
          c_nodes[out_id] = std::move(rounded);
          has_c[out_id] = true;
        }
        cross_plane[out_id] = ((size_t)in_id < cross_plane.size()) ? cross_plane[in_id] : default_sketch_plane();
        sem.inputs = {in_id};
        sem.params_f64 = {radius};
      } break;
      case OpCode::CrossFilletCorners: {
        uint32_t in_id = 0;
        uint32_t corner_count = 0;
        if (!read_u32(&payload, &in_id) || !read_u32(&payload, &corner_count) || corner_count == 0) {
          *error = "Replay failed: invalid cross fillet corners payload.";
          return false;
        }
        manifold::CrossSection in_c;
        if (!need_c(c_nodes, has_c, in_id, &in_c, error)) return false;

        std::vector<CornerFilletSpec> specs;
        specs.reserve(corner_count);
        sem.params_u32.push_back(corner_count);
        for (uint32_t i = 0; i < corner_count; ++i) {
          CornerFilletSpec spec = {};
          if (!read_u32(&payload, &spec.contour) || !read_u32(&payload, &spec.vertex) ||
              !read_f64(&payload, &spec.radius)) {
            *error = "Replay failed: invalid cross fillet corner entry payload.";
            return false;
          }
          if (!std::isfinite(spec.radius) || spec.radius < 0.0) {
            *error = "Replay failed: cross fillet corner radius must be finite and >= 0.";
            return false;
          }
          specs.push_back(spec);
          sem.params_u32.push_back(spec.contour);
          sem.params_u32.push_back(spec.vertex);
          sem.params_f64.push_back(spec.radius);
        }

        manifold::CrossSection out_c;
        if (!apply_corner_fillets(in_c, specs, lod_policy, &out_c, error)) return false;
        c_nodes[out_id] = std::move(out_c);
        has_c[out_id] = true;
        cross_plane[out_id] =
            ((size_t)in_id < cross_plane.size()) ? cross_plane[in_id] : default_sketch_plane();
        sem.inputs = {in_id};
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
        cross_plane[out_id] = ((size_t)in_id < cross_plane.size()) ? cross_plane[in_id] : default_sketch_plane();
        sem.inputs = {in_id};
        sem.params_f64 = {delta};
      } break;
      case OpCode::CrossPlane: {
        uint32_t in_id = 0;
        uint32_t kind_u32 = 0;
        double offset = 0.0;
        if (!read_u32(&payload, &in_id) || !read_u32(&payload, &kind_u32) || !read_f64(&payload, &offset)) {
          *error = "Replay failed: invalid cross plane payload.";
          return false;
        }
        if (!valid_plane_kind(kind_u32)) {
          *error = "Replay failed: invalid cross plane kind.";
          return false;
        }
        if (!std::isfinite(offset)) {
          *error = "Replay failed: invalid cross plane offset.";
          return false;
        }
        manifold::CrossSection in_c;
        if (!need_c(c_nodes, has_c, in_id, &in_c, error)) return false;
        c_nodes[out_id] = in_c;
        has_c[out_id] = true;
        cross_plane[out_id] = {static_cast<SketchPlaneKind>(kind_u32), offset};
        sem.inputs = {in_id};
        sem.params_u32 = {kind_u32};
        sem.params_f64 = {offset};
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
        const SketchPlane plane =
            ((size_t)cs_id < cross_plane.size()) ? cross_plane[cs_id] : default_sketch_plane();
        m = apply_plane_to_manifold(m, plane);
        if (!check_status(m, "extrude", error)) return false;
        m_nodes[out_id] = std::move(m);
        has_m[out_id] = true;
        sem.inputs = {cs_id};
        sem.params_f64 = {h, twist};
        sem.params_u32 = {div};
      } break;
      case OpCode::Revolve: {
        uint32_t cs_id = 0, ignored_segments = 0;
        double deg = 0.0;
        if (!read_u32(&payload, &cs_id) || !read_u32(&payload, &ignored_segments) || !read_f64(&payload, &deg)) {
          *error = "Replay failed: invalid revolve payload.";
          return false;
        }
        manifold::CrossSection cs;
        if (!need_c(c_nodes, has_c, cs_id, &cs, error)) return false;
        const manifold::Polygons polys = cs.ToPolygons();
        const double radius = revolve_effective_radius(polys);
        const uint32_t seg =
            (uint32_t)AutoCircularSegmentsForRevolve(radius, deg, lod_policy.profile);
        manifold::Manifold m = manifold::Manifold::Revolve(polys, (int)seg, deg);
        const SketchPlane plane =
            ((size_t)cs_id < cross_plane.size()) ? cross_plane[cs_id] : default_sketch_plane();
        m = apply_plane_to_manifold(m, plane);
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
        cross_plane[out_id] = default_sketch_plane();
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
                           const ReplayLodPolicy &lod_policy,
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
  manifold::Manifold post = ApplyReplayPostprocess(m, lod_policy.postprocess);
  if (!check_status(post, "postprocess", error)) return false;
  *out = std::move(post);
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

bool ResolveReplayCrossSectionPlane(const ReplayTables &tables, uint32_t root_kind, uint32_t root_id,
                                    SketchPlane *out, std::string *error) {
  if (!out) {
    *error = "Replay failed: null cross-section plane output.";
    return false;
  }
  if (root_kind != (uint32_t)NodeKind::CrossSection) {
    *error = "Replay failed: root node is not a cross-section.";
    return false;
  }
  if ((size_t)root_id >= tables.cross_nodes.size() || !tables.has_cross[root_id]) {
    *error = "Replay failed: root cross-section node missing.";
    return false;
  }
  if ((size_t)root_id >= tables.cross_plane.size()) {
    *out = default_sketch_plane();
    return true;
  }
  *out = tables.cross_plane[root_id];
  return true;
}

bool ReplayOpsToMesh(const ReplayInput &in, manifold::MeshGL *mesh, std::string *error) {
  ReplayTables tables;
  if (!ReplayOpsToTables(in.records, in.records_size, in.op_count,
                         in.lod_policy, &tables, error)) {
    return false;
  }
  manifold::Manifold out;
  if (!ResolveReplayManifold(tables, in.root_kind, in.root_id,
                             in.lod_policy, &out, error)) {
    return false;
  }
  *mesh = out.GetMeshGL();
  return true;
}

}  // namespace vicad
