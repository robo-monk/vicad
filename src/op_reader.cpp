#include "op_reader.h"

#include <cstring>

#include "ipc_protocol.h"

namespace vicad {

bool ReadOpRecords(const uint8_t *records, size_t records_size, uint32_t expected_count,
                   std::vector<OpRecordView> *out, std::string *error) {
  if (!records || !out) {
    if (error) *error = "Replay failed: null op record buffer.";
    return false;
  }
  out->clear();
  size_t off = 0;
  uint32_t parsed = 0;
  while (off < records_size) {
    if (off + sizeof(OpRecordHeader) > records_size) {
      if (error) *error = "Replay failed: truncated op header.";
      return false;
    }
    OpRecordHeader hdr = {};
    std::memcpy(&hdr, records + off, sizeof(hdr));
    off += sizeof(hdr);
    if (off + hdr.payload_len > records_size) {
      if (error) *error = "Replay failed: truncated op payload.";
      return false;
    }
    OpRecordView view = {};
    view.opcode = hdr.opcode;
    view.payload = records + off;
    view.payload_len = hdr.payload_len;
    out->push_back(view);
    off += hdr.payload_len;
    parsed++;
  }
  if (parsed != expected_count) {
    if (error) *error = "Replay failed: op count mismatch.";
    return false;
  }
  return true;
}

}  // namespace vicad
