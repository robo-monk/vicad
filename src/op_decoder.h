#ifndef VICAD_OP_DECODER_H_
#define VICAD_OP_DECODER_H_

#include <cstddef>
#include <cstdint>
#include <string>

#include "manifold/manifold.h"
#include "manifold/cross_section.h"

namespace vicad {

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
