#include "op_decoder.h"

#include <cstring>
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
                 uint32_t id) {
  const size_t need = (size_t)id + 1;
  if (m_nodes->size() >= need) return true;
  m_nodes->resize(need);
  has_m->resize(need, false);
  c_nodes->resize(need);
  has_c->resize(need, false);
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

}  // namespace

bool ReplayOpsToTables(const uint8_t *records, size_t records_size, uint32_t op_count,
                       ReplayTables *tables, std::string *error) {
  tables->manifold_nodes.clear();
  tables->has_manifold.clear();
  tables->cross_nodes.clear();
  tables->has_cross.clear();

  std::vector<manifold::Manifold> &m_nodes = tables->manifold_nodes;
  std::vector<bool> &has_m = tables->has_manifold;
  std::vector<manifold::CrossSection> &c_nodes = tables->cross_nodes;
  std::vector<bool> &has_c = tables->has_cross;

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
    ensure_node(&m_nodes, &has_m, &c_nodes, &has_c, out_id);

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
      } break;
      case OpCode::Union: {
        uint32_t count = 0;
        if (!read_u32(&payload, &count) || count == 0) {
          *error = "Replay failed: invalid union payload.";
          return false;
        }
        std::vector<manifold::Manifold> parts;
        parts.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
          uint32_t id = 0;
          if (!read_u32(&payload, &id)) {
            *error = "Replay failed: invalid union args.";
            return false;
          }
          manifold::Manifold part;
          if (!need_m(m_nodes, has_m, id, &part, error)) return false;
          parts.push_back(part);
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
      } break;
      default:
        *error = "Replay failed: unknown opcode " + std::to_string(hdr.opcode);
        return false;
    }

    if (payload.off != payload.len) {
      *error = "Replay failed: payload trailing bytes for opcode " + std::to_string(hdr.opcode);
      return false;
    }
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

bool ReplayOpsToMesh(const ReplayInput &in, manifold::MeshGL *mesh, std::string *error) {
  ReplayTables tables;
  if (!ReplayOpsToTables(in.records, in.records_size, in.op_count, &tables, error)) return false;
  manifold::Manifold out;
  if (!ResolveReplayManifold(tables, in.root_kind, in.root_id, &out, error)) return false;
  *mesh = out.GetMeshGL();
  return true;
}

}  // namespace vicad
