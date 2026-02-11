#ifndef VICAD_OP_DECODER_H_
#define VICAD_OP_DECODER_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "manifold/manifold.h"
#include "manifold/cross_section.h"
#include "sketch_dimensions.h"

namespace vicad {

struct ReplayNodeSemantic {
  uint16_t opcode = 0;
  uint32_t out_id = 0;
  std::vector<uint32_t> inputs;
  std::vector<double> params_f64;
  std::vector<uint32_t> params_u32;
  manifold::Polygons polygons;
  bool has_polygons = false;
  bool valid = false;
};

struct ReplayTables {
  std::vector<manifold::Manifold> manifold_nodes;
  std::vector<bool> has_manifold;
  std::vector<manifold::CrossSection> cross_nodes;
  std::vector<bool> has_cross;
  std::vector<ReplayNodeSemantic> node_semantics;
};

bool ReplayOpsToTables(const uint8_t *records, size_t records_size, uint32_t op_count,
                       ReplayTables *tables, std::string *error);
bool ResolveReplayManifold(const ReplayTables &tables, uint32_t root_kind, uint32_t root_id,
                           manifold::Manifold *out, std::string *error);
bool ResolveReplayCrossSection(const ReplayTables &tables, uint32_t root_kind, uint32_t root_id,
                               manifold::CrossSection *out, std::string *error);
bool BuildSketchDimensionModelForRoot(const ReplayTables &tables, uint32_t root_id,
                                      SketchDimensionModel *out, std::string *error);
bool BuildOperationTraceForRoot(const ReplayTables &tables, uint32_t root_kind, uint32_t root_id,
                                std::vector<OpTraceEntry> *out, std::string *error);

struct ReplayInput {
  const uint8_t *records;
  size_t records_size;
  uint32_t op_count;
  uint32_t root_kind;
  uint32_t root_id;
};
bool ReplayOpsToMesh(const ReplayInput &in, manifold::MeshGL *mesh, std::string *error);

}  // namespace vicad

#endif  // VICAD_OP_DECODER_H_
