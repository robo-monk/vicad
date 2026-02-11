#ifndef VICAD_OP_DECODER_H_
#define VICAD_OP_DECODER_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "manifold/manifold.h"
#include "manifold/cross_section.h"

namespace vicad {

struct ReplayTables {
  std::vector<manifold::Manifold> manifold_nodes;
  std::vector<bool> has_manifold;
  std::vector<manifold::CrossSection> cross_nodes;
  std::vector<bool> has_cross;
};

bool ReplayOpsToTables(const uint8_t *records, size_t records_size, uint32_t op_count,
                       ReplayTables *tables, std::string *error);
bool ResolveReplayManifold(const ReplayTables &tables, uint32_t root_kind, uint32_t root_id,
                           manifold::Manifold *out, std::string *error);

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
