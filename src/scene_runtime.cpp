#include "scene_runtime.h"

#include <cstdio>
#include <ctime>

namespace vicad_runtime {

std::string MakeExport3mfFilename() {
  std::time_t now = std::time(nullptr);
  std::tm tmv = {};
#if defined(_WIN32)
  localtime_s(&tmv, &now);
#else
  localtime_r(&now, &tmv);
#endif
  char buf[128];
  std::snprintf(buf, sizeof(buf),
                "vicad-export-%04d%02d%02d-%02d%02d%02d.3mf",
                tmv.tm_year + 1900,
                tmv.tm_mon + 1,
                tmv.tm_mday,
                tmv.tm_hour,
                tmv.tm_min,
                tmv.tm_sec);
  return std::string(buf);
}

}  // namespace vicad_runtime
