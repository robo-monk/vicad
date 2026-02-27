#ifndef VICAD_LOG_H_
#define VICAD_LOG_H_

#include <cstdint>
#include <cstdio>
#include <cstring>

// Emits a single newline-delimited JSON log record to stderr.
//
// Format:
//   {"src":"vicad","event":"<event>","run_id":<run_id>}
//   {"src":"vicad","event":"<event>","run_id":<run_id>,"details":"<escaped>"}
//
// The details string is JSON-escaped: backslashes, quotes, and control
// characters are all safely encoded so the output is always valid JSON.
//
// Agents can query logs with:
//   ./vicad 2>build/vicad.log
//   grep '"event":"RUN_DONE"' build/vicad.log | jq .
//
// Usage:
//   vicad::log_event("RUN_DONE", seq);
//   vicad::log_event("RUN_DONE", seq, "duration_ms=123");
//   vicad::log_event("SCRIPT_ERROR", 0, err.c_str());  // multiline strings are safe

namespace vicad {

inline void log_event(const char *event, uint64_t run_id, const char *details = nullptr) {
  if (details && details[0] != '\0') {
    // Write prefix
    std::fprintf(stderr, "{\"src\":\"vicad\",\"event\":\"%s\",\"run_id\":%llu,\"details\":\"",
                 event, (unsigned long long)run_id);
    // JSON-escape details character by character
    for (const char *p = details; *p != '\0'; ++p) {
      const unsigned char c = (unsigned char)*p;
      if      (c == '"')  std::fputs("\\\"", stderr);
      else if (c == '\\') std::fputs("\\\\", stderr);
      else if (c == '\n') std::fputs("\\n",  stderr);
      else if (c == '\r') std::fputs("\\r",  stderr);
      else if (c == '\t') std::fputs("\\t",  stderr);
      else if (c < 0x20)  std::fprintf(stderr, "\\u%04x", c);
      else                std::fputc(c, stderr);
    }
    std::fputs("\"}\n", stderr);
  } else {
    std::fprintf(stderr, "{\"src\":\"vicad\",\"event\":\"%s\",\"run_id\":%llu}\n",
                 event, (unsigned long long)run_id);
  }
}

}  // namespace vicad

#endif  // VICAD_LOG_H_
