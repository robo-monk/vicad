#include "op_decoder.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
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

std::vector<uint8_t> payload_cross_offset_clone(uint32_t out_id, uint32_t in_id,
                                                double delta) {
  std::vector<uint8_t> out;
  append_pod(&out, out_id);
  append_pod(&out, in_id);
  append_pod(&out, delta);
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
    // Explicit segment override must be stable across profiles.
    std::vector<uint8_t> rec;
    append_record(&rec, vicad::OpCode::Sphere, payload_sphere(1, 20.0, 64));

    manifold::MeshGL d, m, e;
    std::string err;
    ok = ok && require(replay_to_mesh(rec, 1, 1, vicad::LodProfile::Draft, &d, &err),
                       "explicit sphere draft");
    ok = ok && require(replay_to_mesh(rec, 1, 1, vicad::LodProfile::Model, &m, &err),
                       "explicit sphere model");
    ok = ok && require(replay_to_mesh(rec, 1, 1, vicad::LodProfile::Export3MF, &e, &err),
                       "explicit sphere export");
    ok = ok && require(d.NumTri() == m.NumTri() && m.NumTri() == e.NumTri(),
                       "explicit segments unchanged across profiles");
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

  if (!ok) return 1;
  std::cout << "[lod_replay_test] PASS\n";
  return 0;
}
