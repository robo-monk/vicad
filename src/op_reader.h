#ifndef VICAD_OP_READER_H_
#define VICAD_OP_READER_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vicad {

struct OpRecordView {
  uint16_t opcode = 0;
  const uint8_t *payload = nullptr;
  uint32_t payload_len = 0;
};

bool ReadOpRecords(const uint8_t *records, size_t records_size, uint32_t expected_count,
                   std::vector<OpRecordView> *out, std::string *error);

}  // namespace vicad

#endif  // VICAD_OP_READER_H_
