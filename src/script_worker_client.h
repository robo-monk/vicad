#ifndef VICAD_SCRIPT_WORKER_CLIENT_H_
#define VICAD_SCRIPT_WORKER_CLIENT_H_

#include <cstdint>
#include <string>
#include <vector>

#include "manifold/manifold.h"

namespace vicad {

struct SceneVec3 {
  float x;
  float y;
  float z;
};

struct ScriptSceneObject {
  uint64_t objectId;
  std::string name;
  manifold::Manifold manifold;
  manifold::MeshGL mesh;
  SceneVec3 bmin;
  SceneVec3 bmax;
};

class ScriptWorkerClient {
 public:
  ScriptWorkerClient();
  ~ScriptWorkerClient();

  ScriptWorkerClient(const ScriptWorkerClient &) = delete;
  ScriptWorkerClient &operator=(const ScriptWorkerClient &) = delete;

  bool ExecuteScript(const char *script_path, manifold::MeshGL *mesh, std::string *error);
  bool ExecuteScriptScene(const char *script_path, std::vector<ScriptSceneObject> *objects, std::string *error);
  bool started() const { return started_; }
  void Shutdown();

 private:
  bool Start(std::string *error);
  bool CreateSharedMemory(std::string *error);
  bool CreateSocket(std::string *error);
  bool SpawnWorker(std::string *error);
  bool AcceptWorker(std::string *error);
  bool SendLine(const std::string &line, std::string *error);
  bool ReadLineWithTimeout(int timeout_ms, std::string *out, std::string *error);

  bool started_;
  int shm_fd_;
  void *shm_ptr_;
  size_t shm_size_;
  int listen_fd_;
  int conn_fd_;
  int worker_pid_;
  uint64_t next_seq_;
  std::string shm_name_;
  std::string socket_path_;
};

}  // namespace vicad

#endif  // VICAD_SCRIPT_WORKER_CLIENT_H_
