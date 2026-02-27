// run_script.cpp
//
// Runs a single .vicad.ts script through the full IPC path and reports the
// result as a JSON line on stdout.  Worker lifecycle events go to stderr
// (structured JSON via vicad::log_event).
//
// Usage:  build/run_script <path/to/script.vicad.ts>
//
// Exit codes:
//   0  — script executed successfully; stdout contains a "pass" JSON line
//   1  — script failed or usage error; stdout contains a "fail" JSON line

#include <cstdio>
#include <string>
#include <vector>

#include "lod_policy.h"
#include "script_worker_client.h"

namespace {

void json_str(const char *s) {
  for (const char *p = s; *p != '\0'; ++p) {
    const unsigned char c = (unsigned char)*p;
    if      (c == '"')  std::fputs("\\\"", stdout);
    else if (c == '\\') std::fputs("\\\\", stdout);
    else if (c == '\n') std::fputs("\\n",  stdout);
    else if (c == '\r') std::fputs("\\r",  stdout);
    else if (c == '\t') std::fputs("\\t",  stdout);
    else if (c < 0x20)  std::fprintf(stdout, "\\u%04x", c);
    else                std::fputc(c, stdout);
  }
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: run_script <script.vicad.ts>\n");
    std::fprintf(stdout, "{\"result\":\"fail\",\"error\":\"missing script argument\"}\n");
    return 1;
  }
  const char *script = argv[1];

  vicad::ScriptWorkerClient client;
  std::vector<vicad::ScriptSceneObject> objects;
  std::string error;
  vicad::ReplayLodPolicy lod = {};
  lod.profile = vicad::LodProfile::Model;

  const bool ok = client.ExecuteScriptScene(script, &objects, &error, lod);

  if (ok) {
    std::fprintf(stdout, "{\"result\":\"pass\",\"script\":\"");
    json_str(script);
    std::fprintf(stdout, "\",\"objects\":%zu}\n", objects.size());
    return 0;
  } else {
    std::fprintf(stdout, "{\"result\":\"fail\",\"script\":\"");
    json_str(script);
    std::fprintf(stdout, "\",\"error\":\"");
    json_str(error.c_str());
    std::fprintf(stdout, "\"}\n");
    return 1;
  }
}
