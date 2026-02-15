#include "op_decoder.h"

#include <string>
#include <unordered_set>
#include <vector>

#include "ipc_protocol.h"

namespace vicad {

namespace {

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
    case OpCode::CrossPlane: return "CrossPlane";
    default: return "Unknown";
  }
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

}  // namespace vicad
