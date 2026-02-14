#ifndef VICAD_SCRIPT_WORKER_CLIENT_H_
#define VICAD_SCRIPT_WORKER_CLIENT_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "manifold/manifold.h"
#include "lod_policy.h"
#include "sketch_dimensions.h"

namespace vicad {

enum class ScriptSceneObjectKind : uint32_t {
  Unknown = 0,
  Manifold = 1,
  CrossSection = 2,
};

struct SceneVec3 {
  float x;
  float y;
  float z;
};

struct ScriptSketchContour {
  std::vector<SceneVec3> points;
};

struct ScriptSceneObject {
  uint64_t objectId = 0;
  std::string name;
  ScriptSceneObjectKind kind = ScriptSceneObjectKind::Unknown;
  uint32_t rootKind = 0;
  uint32_t rootId = 0;
  manifold::Manifold manifold;
  manifold::MeshGL mesh;
  std::vector<ScriptSketchContour> sketchContours;
  std::optional<SketchDimensionModel> sketchDims;
  std::vector<OpTraceEntry> opTrace;
  SceneVec3 bmin = {0.0f, 0.0f, 0.0f};
  SceneVec3 bmax = {0.0f, 0.0f, 0.0f};
};

struct ScriptExecutionDiagnostic {
  uint32_t errorCode = 0;
  uint32_t phase = 0;
  uint32_t line = 0;
  uint32_t column = 0;
  uint64_t runId = 0;
  uint32_t durationMs = 0;
  std::string file;
  std::string message;
  std::string stack;
};

class ScriptWorkerClient {
 public:
  ScriptWorkerClient();
  ~ScriptWorkerClient();

  ScriptWorkerClient(const ScriptWorkerClient &) = delete;
  ScriptWorkerClient &operator=(const ScriptWorkerClient &) = delete;

  bool ExecuteScriptScene(const char *script_path,
                          std::vector<ScriptSceneObject> *objects,
                          std::string *error,
                          const ReplayLodPolicy &lod_policy = {});
  bool started() const { return started_; }
  const ScriptExecutionDiagnostic &last_diagnostic() const { return last_diagnostic_; }
  void Shutdown();

 private:
  bool Start(std::string *error);
  bool CreateSharedMemory(std::string *error);
  bool CreateSocket(std::string *error);
  bool SpawnWorker(std::string *error);
  bool AcceptWorker(std::string *error);
  bool SendLine(const std::string &line, std::string *error);
  bool ReadLineWithTimeout(int timeout_ms, std::string *out, std::string *error);
  void LogEvent(const char *event, uint64_t run_id, const std::string &details = "");

  bool started_;
  int shm_fd_;
  void *shm_ptr_;
  size_t shm_size_;
  int listen_fd_;
  int conn_fd_;
  int worker_pid_;
  uint64_t next_seq_;
  ScriptExecutionDiagnostic last_diagnostic_;
  std::string shm_name_;
  std::string socket_path_;
};

}  // namespace vicad

#endif  // VICAD_SCRIPT_WORKER_CLIENT_H_
