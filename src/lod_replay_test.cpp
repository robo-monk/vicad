#include "op_decoder.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <tuple>
#include <vector>

#include "ipc_protocol.h"

namespace {

template <typename T>
void append_pod(std::vector<uint8_t>* out, const T& v) {
  const size_t at = out->size();
  out->resize(at + sizeof(T));
  std::memcpy(out->data() + at, &v, sizeof(T));
}

void append_record(std::vector<uint8_t>* out, vicad::OpCode opcode,
                   const std::vector<uint8_t>& payload) {
  vicad::OpRecordHeader hdr = {};
  hdr.opcode = (uint16_t)opcode;
  hdr.flags = 0;
  hdr.payload_len = (uint32_t)payload.size();
  append_pod(out, hdr);
  out->insert(out->end(), payload.begin(), payload.end());
}

std::vector<uint8_t> payload_sphere(uint32_t out_id, double radius,
                                    uint32_t segments) {
  std::vector<uint8_t> out;
  append_pod(&out, out_id);
  append_pod(&out, radius);
  append_pod(&out, segments);
  return out;
}

std::vector<uint8_t> payload_cylinder(uint32_t out_id, double h, double r1,
                                      double r2, uint32_t segments,
                                      uint32_t center) {
  std::vector<uint8_t> out;
  append_pod(&out, out_id);
  append_pod(&out, h);
  append_pod(&out, r1);
  append_pod(&out, r2);
  append_pod(&out, segments);
  append_pod(&out, center);
  return out;
}

std::vector<uint8_t> payload_cube(uint32_t out_id, double x, double y, double z,
                                  uint32_t center) {
  std::vector<uint8_t> out;
  append_pod(&out, out_id);
  append_pod(&out, x);
  append_pod(&out, y);
  append_pod(&out, z);
  append_pod(&out, center);
  return out;
}

std::vector<uint8_t> payload_cross_circle(uint32_t out_id, double radius,
                                          uint32_t segments) {
  std::vector<uint8_t> out;
  append_pod(&out, out_id);
  append_pod(&out, radius);
  append_pod(&out, segments);
  return out;
}

std::vector<uint8_t> payload_revolve(uint32_t out_id, uint32_t cs_id,
                                     uint32_t segments, double degrees) {
  std::vector<uint8_t> out;
  append_pod(&out, out_id);
  append_pod(&out, cs_id);
  append_pod(&out, segments);
  append_pod(&out, degrees);
  return out;
}

std::vector<uint8_t> payload_cross_square(uint32_t out_id, double w, double h,
                                          uint32_t center) {
  std::vector<uint8_t> out;
  append_pod(&out, out_id);
  append_pod(&out, w);
  append_pod(&out, h);
  append_pod(&out, center);
  return out;
}

std::vector<uint8_t> payload_cross_fillet(uint32_t out_id, uint32_t in_id,
                                          double radius) {
  std::vector<uint8_t> out;
  append_pod(&out, out_id);
  append_pod(&out, in_id);
  append_pod(&out, radius);
  return out;
}

std::vector<uint8_t> payload_cross_fillet_corners(
    uint32_t out_id, uint32_t in_id,
    const std::vector<std::tuple<uint32_t, uint32_t, double>> &corners) {
  std::vector<uint8_t> out;
  append_pod(&out, out_id);
  append_pod(&out, in_id);
  const uint32_t count = (uint32_t)corners.size();
  append_pod(&out, count);
  for (const auto &corner : corners) {
    append_pod(&out, std::get<0>(corner));
    append_pod(&out, std::get<1>(corner));
    append_pod(&out, std::get<2>(corner));
  }
  return out;
}

std::vector<uint8_t> payload_cross_offset_clone(uint32_t out_id, uint32_t in_id,
                                                double delta) {
  std::vector<uint8_t> out;
  append_pod(&out, out_id);
  append_pod(&out, in_id);
  append_pod(&out, delta);
  return out;
}

std::vector<uint8_t> payload_cross_plane(uint32_t out_id, uint32_t in_id,
                                         uint32_t kind, double offset) {
  std::vector<uint8_t> out;
  append_pod(&out, out_id);
  append_pod(&out, in_id);
  append_pod(&out, kind);
  append_pod(&out, offset);
  return out;
}

std::vector<uint8_t> payload_cross_translate(uint32_t out_id, uint32_t in_id,
                                             double x, double y) {
  std::vector<uint8_t> out;
  append_pod(&out, out_id);
  append_pod(&out, in_id);
  append_pod(&out, x);
  append_pod(&out, y);
  return out;
}

std::vector<uint8_t> payload_extrude(uint32_t out_id, uint32_t cs_id, double h,
                                     uint32_t divisions, double twist) {
  std::vector<uint8_t> out;
  append_pod(&out, out_id);
  append_pod(&out, cs_id);
  append_pod(&out, h);
  append_pod(&out, divisions);
  append_pod(&out, twist);
  return out;
}

bool replay_to_mesh(const std::vector<uint8_t>& records, uint32_t op_count,
                    uint32_t root_id, vicad::LodProfile profile,
                    manifold::MeshGL* out_mesh, std::string* out_err) {
  vicad::ReplayInput in = {};
  in.records = records.data();
  in.records_size = records.size();
  in.op_count = op_count;
  in.root_kind = (uint32_t)vicad::NodeKind::Manifold;
  in.root_id = root_id;
  in.lod_policy.profile = profile;
  return vicad::ReplayOpsToMesh(in, out_mesh, out_err);
}

bool require(bool cond, const char* msg) {
  if (cond) return true;
  std::cerr << "[lod_replay_test] FAIL: " << msg << "\n";
  return false;
}

bool mesh_bounds(const manifold::MeshGL &mesh, manifold::vec3 *out_min, manifold::vec3 *out_max) {
  if (!out_min || !out_max) return false;
  if (mesh.numProp < 3 || mesh.vertProperties.empty()) return false;
  const size_t count = mesh.vertProperties.size() / mesh.numProp;
  if (count == 0) return false;
  manifold::vec3 bmin(1e30, 1e30, 1e30);
  manifold::vec3 bmax(-1e30, -1e30, -1e30);
  for (size_t i = 0; i < count; ++i) {
    const size_t at = i * mesh.numProp;
    const double x = mesh.vertProperties[at + 0];
    const double y = mesh.vertProperties[at + 1];
    const double z = mesh.vertProperties[at + 2];
    bmin.x = std::min(bmin.x, x);
    bmin.y = std::min(bmin.y, y);
    bmin.z = std::min(bmin.z, z);
    bmax.x = std::max(bmax.x, x);
    bmax.y = std::max(bmax.y, y);
    bmax.z = std::max(bmax.z, z);
  }
  *out_min = bmin;
  *out_max = bmax;
  return true;
}

}  // namespace

int main() {
  bool ok = true;

  {
    // Sphere auto segmentation should increase with profile quality.
    std::vector<uint8_t> rec;
    append_record(&rec, vicad::OpCode::Sphere, payload_sphere(1, 20.0, 0));

    manifold::MeshGL d, m, e;
    std::string err;
    ok = ok && require(replay_to_mesh(rec, 1, 1, vicad::LodProfile::Draft, &d, &err),
                       "sphere replay draft");
    ok = ok && require(replay_to_mesh(rec, 1, 1, vicad::LodProfile::Model, &m, &err),
                       "sphere replay model");
    ok = ok && require(replay_to_mesh(rec, 1, 1, vicad::LodProfile::Export3MF, &e, &err),
                       "sphere replay export");
    ok = ok && require(d.NumTri() < m.NumTri() && m.NumTri() < e.NumTri(),
                       "sphere tri count Draft < Model < Export3MF");
  }

  {
    // Cylinder auto segmentation should increase with profile quality.
    std::vector<uint8_t> rec;
    append_record(&rec, vicad::OpCode::Cylinder,
                  payload_cylinder(1, 20.0, 8.0, -1.0, 0, 0));

    manifold::MeshGL d, m, e;
    std::string err;
    ok = ok && require(replay_to_mesh(rec, 1, 1, vicad::LodProfile::Draft, &d, &err),
                       "cylinder replay draft");
    ok = ok && require(replay_to_mesh(rec, 1, 1, vicad::LodProfile::Model, &m, &err),
                       "cylinder replay model");
    ok = ok && require(replay_to_mesh(rec, 1, 1, vicad::LodProfile::Export3MF, &e, &err),
                       "cylinder replay export");
    ok = ok && require(d.NumTri() < m.NumTri() && m.NumTri() < e.NumTri(),
                       "cylinder tri count Draft < Model < Export3MF");
  }

  {
    // Revolve auto segmentation should increase with profile quality.
    std::vector<uint8_t> rec;
    append_record(&rec, vicad::OpCode::CrossCircle, payload_cross_circle(1, 6.0, 0));
    append_record(&rec, vicad::OpCode::Revolve, payload_revolve(2, 1, 0, 360.0));

    manifold::MeshGL d, m, e;
    std::string err;
    ok = ok && require(replay_to_mesh(rec, 2, 2, vicad::LodProfile::Draft, &d, &err),
                       "revolve replay draft");
    ok = ok && require(replay_to_mesh(rec, 2, 2, vicad::LodProfile::Model, &m, &err),
                       "revolve replay model");
    ok = ok && require(replay_to_mesh(rec, 2, 2, vicad::LodProfile::Export3MF, &e, &err),
                       "revolve replay export");
    ok = ok && require(d.NumTri() < m.NumTri() && m.NumTri() < e.NumTri(),
                       "revolve tri count Draft < Model < Export3MF");
  }

  {
    // Any encoded segment field is ignored; profile-driven auto LOD is canonical.
    std::vector<uint8_t> rec;
    append_record(&rec, vicad::OpCode::Sphere, payload_sphere(1, 20.0, 64));

    manifold::MeshGL d, m, e;
    std::string err;
    ok = ok && require(replay_to_mesh(rec, 1, 1, vicad::LodProfile::Draft, &d, &err),
                       "ignored explicit sphere draft");
    ok = ok && require(replay_to_mesh(rec, 1, 1, vicad::LodProfile::Model, &m, &err),
                       "ignored explicit sphere model");
    ok = ok && require(replay_to_mesh(rec, 1, 1, vicad::LodProfile::Export3MF, &e, &err),
                       "ignored explicit sphere export");
    ok = ok && require(d.NumTri() < m.NumTri() && m.NumTri() < e.NumTri(),
                       "profile LOD wins even when segment field is non-zero");
  }

  {
    // Non-circular primitives should be unchanged across profiles.
    std::vector<uint8_t> rec;
    append_record(&rec, vicad::OpCode::Cube, payload_cube(1, 4.0, 5.0, 6.0, 0));

    manifold::MeshGL d, m, e;
    std::string err;
    ok = ok && require(replay_to_mesh(rec, 1, 1, vicad::LodProfile::Draft, &d, &err),
                       "cube draft");
    ok = ok && require(replay_to_mesh(rec, 1, 1, vicad::LodProfile::Model, &m, &err),
                       "cube model");
    ok = ok && require(replay_to_mesh(rec, 1, 1, vicad::LodProfile::Export3MF, &e, &err),
                       "cube export");
    ok = ok && require(d.NumTri() == m.NumTri() && m.NumTri() == e.NumTri(),
                       "cube tri count unchanged across profiles");
  }

  {
    // 2D fillet (round offset path) should respond to profile quality.
    std::vector<uint8_t> rec;
    append_record(&rec, vicad::OpCode::CrossSquare,
                  payload_cross_square(1, 40.0, 20.0, 1));
    append_record(&rec, vicad::OpCode::CrossFillet,
                  payload_cross_fillet(2, 1, 5.0));
    append_record(&rec, vicad::OpCode::Extrude,
                  payload_extrude(3, 2, 8.0, 0, 0.0));

    manifold::MeshGL d, m, e;
    std::string err;
    ok = ok && require(replay_to_mesh(rec, 3, 3, vicad::LodProfile::Draft, &d, &err),
                       "fillet draft");
    ok = ok && require(replay_to_mesh(rec, 3, 3, vicad::LodProfile::Model, &m, &err),
                       "fillet model");
    ok = ok && require(replay_to_mesh(rec, 3, 3, vicad::LodProfile::Export3MF, &e, &err),
                       "fillet export");
    ok = ok && require(d.NumTri() < m.NumTri() && m.NumTri() < e.NumTri(),
                       "fillet tri count Draft < Model < Export3MF");
  }

  {
    // Per-corner 2D fillet should respond to profile quality.
    std::vector<uint8_t> rec;
    append_record(&rec, vicad::OpCode::CrossSquare,
                  payload_cross_square(1, 40.0, 20.0, 1));
    append_record(&rec, vicad::OpCode::CrossFilletCorners,
                  payload_cross_fillet_corners(
                      2, 1, {{0u, 0u, 4.0}, {0u, 2u, 2.0}}));
    append_record(&rec, vicad::OpCode::Extrude,
                  payload_extrude(3, 2, 8.0, 0, 0.0));

    manifold::MeshGL d, m, e;
    std::string err;
    ok = ok && require(replay_to_mesh(rec, 3, 3, vicad::LodProfile::Draft, &d, &err),
                       "fillet corners draft");
    ok = ok && require(replay_to_mesh(rec, 3, 3, vicad::LodProfile::Model, &m, &err),
                       "fillet corners model");
    ok = ok && require(replay_to_mesh(rec, 3, 3, vicad::LodProfile::Export3MF, &e, &err),
                       "fillet corners export");
    ok = ok && require(d.NumTri() < m.NumTri() && m.NumTri() < e.NumTri(),
                       "fillet corners tri count Draft < Model < Export3MF");
  }

  {
    // Miter offset clone remains profile-invariant.
    std::vector<uint8_t> rec;
    append_record(&rec, vicad::OpCode::CrossSquare,
                  payload_cross_square(1, 40.0, 20.0, 1));
    append_record(&rec, vicad::OpCode::CrossOffsetClone,
                  payload_cross_offset_clone(2, 1, 4.0));
    append_record(&rec, vicad::OpCode::Extrude,
                  payload_extrude(3, 2, 8.0, 0, 0.0));

    manifold::MeshGL d, m, e;
    std::string err;
    ok = ok && require(replay_to_mesh(rec, 3, 3, vicad::LodProfile::Draft, &d, &err),
                       "offset clone draft");
    ok = ok && require(replay_to_mesh(rec, 3, 3, vicad::LodProfile::Model, &m, &err),
                       "offset clone model");
    ok = ok && require(replay_to_mesh(rec, 3, 3, vicad::LodProfile::Export3MF, &e, &err),
                       "offset clone export");
    ok = ok && require(d.NumTri() == m.NumTri() && m.NumTri() == e.NumTri(),
                       "offset clone tri count unchanged across profiles");
  }

  {
    // XZ plane extrude should advance along +Y.
    std::vector<uint8_t> rec;
    append_record(&rec, vicad::OpCode::CrossSquare,
                  payload_cross_square(1, 10.0, 10.0, 1));
    append_record(&rec, vicad::OpCode::CrossPlane,
                  payload_cross_plane(2, 1, 1, 0.0));
    append_record(&rec, vicad::OpCode::Extrude,
                  payload_extrude(3, 2, 10.0, 0, 0.0));

    manifold::MeshGL mesh;
    std::string err;
    ok = ok && require(replay_to_mesh(rec, 3, 3, vicad::LodProfile::Model, &mesh, &err),
                       "xz extrude replay");
    manifold::vec3 bmin, bmax;
    ok = ok && require(mesh_bounds(mesh, &bmin, &bmax), "xz extrude bounds");
    ok = ok && require(std::fabs(bmin.y - 0.0) < 1e-6 && std::fabs(bmax.y - 10.0) < 1e-6,
                       "xz extrude maps height to +Y");
    ok = ok && require(std::fabs((bmax.x - bmin.x) - 10.0) < 1e-6 &&
                       std::fabs((bmax.z - bmin.z) - 10.0) < 1e-6,
                       "xz extrude keeps profile extents in X/Z");
  }

  {
    // YZ plane extrude should advance along +X from offset.
    std::vector<uint8_t> rec;
    append_record(&rec, vicad::OpCode::CrossSquare,
                  payload_cross_square(1, 8.0, 6.0, 1));
    append_record(&rec, vicad::OpCode::CrossPlane,
                  payload_cross_plane(2, 1, 2, 7.0));
    append_record(&rec, vicad::OpCode::Extrude,
                  payload_extrude(3, 2, 5.0, 0, 0.0));

    manifold::MeshGL mesh;
    std::string err;
    ok = ok && require(replay_to_mesh(rec, 3, 3, vicad::LodProfile::Model, &mesh, &err),
                       "yz extrude replay");
    manifold::vec3 bmin, bmax;
    ok = ok && require(mesh_bounds(mesh, &bmin, &bmax), "yz extrude bounds");
    ok = ok && require(std::fabs(bmin.x - 7.0) < 1e-6 && std::fabs(bmax.x - 12.0) < 1e-6,
                       "yz extrude maps height to +X with offset");
    ok = ok && require(std::fabs((bmax.y - bmin.y) - 8.0) < 1e-6 &&
                       std::fabs((bmax.z - bmin.z) - 6.0) < 1e-6,
                       "yz extrude keeps profile extents in Y/Z");
  }

  {
    // CrossPlane metadata should propagate through cross transforms.
    std::vector<uint8_t> rec;
    append_record(&rec, vicad::OpCode::CrossSquare,
                  payload_cross_square(1, 20.0, 20.0, 1));
    append_record(&rec, vicad::OpCode::CrossPlane,
                  payload_cross_plane(2, 1, 1, 3.0));
    append_record(&rec, vicad::OpCode::CrossTranslate,
                  payload_cross_translate(3, 2, 4.0, 0.0));
    append_record(&rec, vicad::OpCode::CrossFillet,
                  payload_cross_fillet(4, 3, 2.0));
    append_record(&rec, vicad::OpCode::CrossOffsetClone,
                  payload_cross_offset_clone(5, 4, 1.0));

    vicad::ReplayTables tables;
    std::string err;
    vicad::ReplayLodPolicy lod_policy = {};
    lod_policy.profile = vicad::LodProfile::Model;
    ok = ok && require(vicad::ReplayOpsToTables(rec.data(), rec.size(), 5, lod_policy, &tables, &err),
                       "cross plane replay tables");
    vicad::SketchPlane plane;
    ok = ok && require(vicad::ResolveReplayCrossSectionPlane(
                           tables, (uint32_t)vicad::NodeKind::CrossSection, 5, &plane, &err),
                       "resolve propagated cross plane");
    ok = ok && require((uint32_t)plane.kind == 1 && std::fabs(plane.offset - 3.0) < 1e-9,
                       "cross plane metadata propagated");
  }

  if (!ok) return 1;
  std::cout << "[lod_replay_test] PASS\n";
  return 0;
}
