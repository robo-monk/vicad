#include "script_worker_client.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <string>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cmath>
#include <limits>
#include <vector>

#include "ipc_protocol.h"
#include "op_decoder.h"

namespace vicad {

namespace {

bool set_err(std::string *error, const std::string &msg) {
  if (error) *error = msg;
  return false;
}

bool compute_bounds(const manifold::MeshGL &mesh, SceneVec3 *out_min, SceneVec3 *out_max) {
  if (mesh.numProp < 3 || mesh.vertProperties.empty()) return false;
  const size_t count = mesh.vertProperties.size() / mesh.numProp;
  if (count == 0) return false;
  double minx = std::numeric_limits<double>::infinity();
  double miny = std::numeric_limits<double>::infinity();
  double minz = std::numeric_limits<double>::infinity();
  double maxx = -std::numeric_limits<double>::infinity();
  double maxy = -std::numeric_limits<double>::infinity();
  double maxz = -std::numeric_limits<double>::infinity();
  for (size_t i = 0; i < count; ++i) {
    const size_t b = i * mesh.numProp;
    const double x = mesh.vertProperties[b + 0];
    const double y = mesh.vertProperties[b + 1];
    const double z = mesh.vertProperties[b + 2];
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) continue;
    minx = std::min(minx, x);
    miny = std::min(miny, y);
    minz = std::min(minz, z);
    maxx = std::max(maxx, x);
    maxy = std::max(maxy, y);
    maxz = std::max(maxz, z);
  }
  if (!std::isfinite(minx) || !std::isfinite(maxx)) return false;
  *out_min = {(float)minx, (float)miny, (float)minz};
  *out_max = {(float)maxx, (float)maxy, (float)maxz};
  return true;
}

bool compute_sketch_bounds(const std::vector<ScriptSketchContour> &contours,
                           SceneVec3 *out_min, SceneVec3 *out_max) {
  double minx = std::numeric_limits<double>::infinity();
  double miny = std::numeric_limits<double>::infinity();
  double minz = std::numeric_limits<double>::infinity();
  double maxx = -std::numeric_limits<double>::infinity();
  double maxy = -std::numeric_limits<double>::infinity();
  double maxz = -std::numeric_limits<double>::infinity();
  for (const ScriptSketchContour &contour : contours) {
    for (const SceneVec3 &p : contour.points) {
      const double x = p.x;
      const double y = p.y;
      const double z = p.z;
      if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) continue;
      minx = std::min(minx, x);
      miny = std::min(miny, y);
      minz = std::min(minz, z);
      maxx = std::max(maxx, x);
      maxy = std::max(maxy, y);
      maxz = std::max(maxz, z);
    }
  }
  if (!std::isfinite(minx) || !std::isfinite(maxx)) return false;
  const double pad = 1e-3;
  *out_min = {(float)minx, (float)miny, (float)(minz - pad)};
  *out_max = {(float)maxx, (float)maxy, (float)(maxz + pad)};
  return true;
}

bool write_all(int fd, const void *buf, size_t len) {
  const uint8_t *p = (const uint8_t *)buf;
  size_t off = 0;
  while (off < len) {
    const ssize_t n = write(fd, p + off, len - off);
    if (n <= 0) {
      if (errno == EINTR) continue;
      return false;
    }
    off += (size_t)n;
  }
  return true;
}

bool read_error_message(const SharedHeader *hdr, const uint8_t *base, std::string *message, std::string *error) {
  if (hdr->response_length < sizeof(ResponsePayloadError)) {
    return set_err(error, "Worker error payload is truncated.");
  }
  if ((size_t)hdr->response_offset + hdr->response_length > (size_t)hdr->capacity_bytes) {
    return set_err(error, "Worker error payload is out of bounds.");
  }
  const uint8_t *payload_ptr = base + hdr->response_offset;
  ResponsePayloadError resp = {};
  std::memcpy(&resp, payload_ptr, sizeof(resp));
  if (resp.version != kIpcVersion) return set_err(error, "Worker error payload has invalid version.");
  const size_t total = sizeof(resp) + (size_t)resp.message_len;
  if (total > hdr->response_length) return set_err(error, "Worker error message is truncated.");
  message->assign((const char *)(payload_ptr + sizeof(resp)), (size_t)resp.message_len);
  return true;
}

}  // namespace

ScriptWorkerClient::ScriptWorkerClient()
    : started_(false),
      shm_fd_(-1),
      shm_ptr_(nullptr),
      shm_size_(kDefaultShmSize),
      listen_fd_(-1),
      conn_fd_(-1),
      worker_pid_(-1),
      next_seq_(1) {}

ScriptWorkerClient::~ScriptWorkerClient() { Shutdown(); }

bool ScriptWorkerClient::CreateSharedMemory(std::string *error) {
  const int pid = (int)getpid();
  shm_name_ = "/vicad-shm-" + std::to_string(pid);

  shm_fd_ = shm_open(shm_name_.c_str(), O_CREAT | O_RDWR, 0600);
  if (shm_fd_ < 0) {
    return set_err(error, std::string("shm_open failed: ") + std::strerror(errno));
  }
  if (ftruncate(shm_fd_, (off_t)shm_size_) != 0) {
    return set_err(error, std::string("ftruncate failed: ") + std::strerror(errno));
  }

  shm_ptr_ = mmap(nullptr, shm_size_, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_, 0);
  if (shm_ptr_ == MAP_FAILED) {
    shm_ptr_ = nullptr;
    return set_err(error, std::string("mmap failed: ") + std::strerror(errno));
  }

  std::memset(shm_ptr_, 0, shm_size_);
  SharedHeader *hdr = (SharedHeader *)shm_ptr_;
  std::memcpy(hdr->magic, kIpcMagic, sizeof(kIpcMagic));
  hdr->version = kIpcVersion;
  hdr->capacity_bytes = (uint32_t)shm_size_;
  hdr->request_seq = 0;
  hdr->response_seq = 0;
  hdr->request_offset = kDefaultRequestOffset;
  hdr->request_length = 0;
  hdr->response_offset = kDefaultResponseOffset;
  hdr->response_length = 0;
  hdr->state = (uint32_t)IpcState::Idle;
  hdr->error_code = (uint32_t)IpcErrorCode::None;
  hdr->reserved = 0;
  return true;
}

bool ScriptWorkerClient::CreateSocket(std::string *error) {
  const int pid = (int)getpid();
  socket_path_ = "/tmp/vicad-worker-" + std::to_string(pid) + ".sock";
  unlink(socket_path_.c_str());

  listen_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    return set_err(error, std::string("socket failed: ") + std::strerror(errno));
  }

  struct sockaddr_un addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  if (socket_path_.size() >= sizeof(addr.sun_path)) {
    return set_err(error, "Socket path is too long.");
  }
  std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path_.c_str());

  if (bind(listen_fd_, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    return set_err(error, std::string("bind failed: ") + std::strerror(errno));
  }
  if (listen(listen_fd_, 1) != 0) {
    return set_err(error, std::string("listen failed: ") + std::strerror(errno));
  }
  return true;
}

bool ScriptWorkerClient::SpawnWorker(std::string *error) {
  const int pid = fork();
  if (pid < 0) {
    return set_err(error, std::string("fork failed: ") + std::strerror(errno));
  }
  if (pid == 0) {
    std::string size_s = std::to_string(shm_size_);
    execlp("bun", "bun", "worker/worker.ts", "--socket", socket_path_.c_str(), "--shm",
           shm_name_.c_str(), "--size", size_s.c_str(), (char *)nullptr);
    _exit(127);
  }
  worker_pid_ = pid;
  return true;
}

bool ScriptWorkerClient::AcceptWorker(std::string *error) {
  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(listen_fd_, &fds);
  struct timeval tv;
  tv.tv_sec = 3;
  tv.tv_usec = 0;
  const int rc = select(listen_fd_ + 1, &fds, nullptr, nullptr, &tv);
  if (rc <= 0) {
    return set_err(error, "Timed out waiting for Bun worker to connect.");
  }
  conn_fd_ = accept(listen_fd_, nullptr, nullptr);
  if (conn_fd_ < 0) {
    return set_err(error, std::string("accept failed: ") + std::strerror(errno));
  }
  return true;
}

bool ScriptWorkerClient::Start(std::string *error) {
  if (started_) return true;
  if (!CreateSharedMemory(error)) return false;
  if (!CreateSocket(error)) return false;
  if (!SpawnWorker(error)) return false;
  if (!AcceptWorker(error)) return false;
  started_ = true;
  return true;
}

bool ScriptWorkerClient::SendLine(const std::string &line, std::string *error) {
  if (conn_fd_ < 0) return set_err(error, "Worker socket is not connected.");
  if (!write_all(conn_fd_, line.data(), line.size())) {
    return set_err(error, std::string("Failed writing socket data: ") + std::strerror(errno));
  }
  return true;
}

bool ScriptWorkerClient::ReadLineWithTimeout(int timeout_ms, std::string *out, std::string *error) {
  out->clear();
  struct pollfd pfd;
  pfd.fd = conn_fd_;
  pfd.events = POLLIN;
  while (true) {
    const int rc = poll(&pfd, 1, timeout_ms);
    if (rc < 0) {
      if (errno == EINTR) continue;
      return set_err(error, std::string("poll failed: ") + std::strerror(errno));
    }
    if (rc == 0) return set_err(error, "Timed out waiting for worker response.");
    if ((pfd.revents & POLLIN) == 0) return set_err(error, "Worker socket closed unexpectedly.");
    char c = 0;
    const ssize_t n = read(conn_fd_, &c, 1);
    if (n <= 0) return set_err(error, "Worker socket read failed.");
    if (c == '\n') return true;
    out->push_back(c);
    if (out->size() > 1024) return set_err(error, "Worker response line too long.");
  }
}

bool ScriptWorkerClient::ExecuteScript(const char *script_path, manifold::MeshGL *mesh, std::string *error) {
  std::vector<ScriptSceneObject> scene_objects;
  if (!ExecuteScriptScene(script_path, &scene_objects, error)) return false;
  std::vector<manifold::Manifold> parts;
  parts.reserve(scene_objects.size());
  for (const ScriptSceneObject &obj : scene_objects) {
    if (obj.kind == ScriptSceneObjectKind::Manifold) {
      parts.push_back(obj.manifold);
    }
  }
  if (parts.empty()) {
    return set_err(error, "Worker returned no manifold scene objects.");
  }
  manifold::Manifold merged = manifold::Manifold::BatchBoolean(parts, manifold::OpType::Add);
  if (merged.Status() != manifold::Manifold::Error::NoError) {
    return set_err(error, "Failed to merge scene objects for legacy mesh output.");
  }
  *mesh = merged.GetMeshGL();
  return true;
}

bool ScriptWorkerClient::ExecuteScriptScene(const char *script_path, std::vector<ScriptSceneObject> *objects,
                                            std::string *error) {
  if (!Start(error)) return false;
  if (!script_path || !objects) return set_err(error, "Invalid execute arguments.");
  objects->clear();

  SharedHeader *hdr = (SharedHeader *)shm_ptr_;
  if (std::memcmp(hdr->magic, kIpcMagic, sizeof(kIpcMagic)) != 0 || hdr->version != kIpcVersion) {
    return set_err(error, "Shared memory header is invalid.");
  }

  const size_t req_cap = (size_t)hdr->response_offset - (size_t)hdr->request_offset;
  const size_t path_len = std::strlen(script_path);
  const size_t req_size = sizeof(RequestPayload) + path_len;
  if (req_size > req_cap) return set_err(error, "Script path is too long for request buffer.");

  uint8_t *base = (uint8_t *)shm_ptr_;
  uint8_t *req = base + hdr->request_offset;
  RequestPayload rp = {};
  rp.version = kIpcVersion;
  rp.script_path_len = (uint32_t)path_len;
  std::memcpy(req, &rp, sizeof(rp));
  std::memcpy(req + sizeof(rp), script_path, path_len);

  const uint64_t seq = next_seq_++;
  hdr->request_seq = seq;
  hdr->request_length = (uint32_t)req_size;
  hdr->response_length = 0;
  hdr->error_code = (uint32_t)IpcErrorCode::None;
  hdr->state = (uint32_t)IpcState::RequestReady;

  if (!SendLine("RUN " + std::to_string(seq) + "\n", error)) return false;

  std::string line;
  if (!ReadLineWithTimeout(30 * 1000, &line, error)) return false;

  const std::string done = "DONE " + std::to_string(seq);
  const std::string fail = "ERROR " + std::to_string(seq);
  if (line == fail) {
    std::string msg;
    std::string read_err;
    if (!read_error_message(hdr, base, &msg, &read_err)) return set_err(error, read_err);
    return set_err(error, msg.empty() ? "Worker reported an error." : msg);
  }
  if (line != done) {
    return set_err(error, "Unexpected worker response: " + line);
  }

  if (hdr->state != (uint32_t)IpcState::ResponseReady) return set_err(error, "Worker state is not ResponseReady.");
  if (hdr->response_seq != seq) return set_err(error, "Worker sequence mismatch.");
  if (hdr->response_length < sizeof(ResponsePayloadScene)) return set_err(error, "Worker response payload is too small.");
  if ((size_t)hdr->response_offset + hdr->response_length > (size_t)hdr->capacity_bytes) {
    return set_err(error, "Worker response payload is out of bounds.");
  }

  const uint8_t *resp_ptr = base + hdr->response_offset;
  ResponsePayloadScene ok = {};
  std::memcpy(&ok, resp_ptr, sizeof(ok));
  if (ok.version != kIpcVersion) {
    return set_err(error, "Worker response version mismatch. Check worker/client protocol compatibility.");
  }

  const size_t payload_need =
      sizeof(ResponsePayloadScene) + (size_t)ok.records_size + (size_t)ok.object_table_size + (size_t)ok.diagnostics_len;
  if (payload_need > hdr->response_length) return set_err(error, "Worker response payload is truncated.");

  const uint8_t *records_ptr = resp_ptr + sizeof(ResponsePayloadScene);
  const uint8_t *object_table_ptr = records_ptr + ok.records_size;
  const uint8_t *names_ptr = object_table_ptr + ok.object_table_size;
  const size_t name_blob_size = (size_t)ok.diagnostics_len;
  const size_t expected_table_size = (size_t)ok.object_count * sizeof(SceneObjectRecord);
  if (ok.object_count == 0) return set_err(error, "Worker returned zero scene objects.");
  if (ok.object_table_size != expected_table_size) {
    return set_err(error, "Worker scene object table size mismatch.");
  }

  ReplayTables tables;
  if (!ReplayOpsToTables(records_ptr, ok.records_size, ok.op_count, &tables, error)) return false;

  size_t name_off = 0;
  objects->reserve(ok.object_count);
  for (uint32_t i = 0; i < ok.object_count; ++i) {
    SceneObjectRecord rec = {};
    std::memcpy(&rec, object_table_ptr + i * sizeof(SceneObjectRecord), sizeof(SceneObjectRecord));
    if (name_off + rec.name_len > name_blob_size) {
      return set_err(error, "Worker scene name blob is truncated.");
    }
    ScriptSceneObject obj;
    obj.objectId = rec.object_id_hash;
    obj.name.assign((const char *)(names_ptr + name_off), (size_t)rec.name_len);
    obj.kind = ScriptSceneObjectKind::Unknown;

    if (rec.root_kind == (uint32_t)NodeKind::Manifold) {
      manifold::Manifold m;
      if (!ResolveReplayManifold(tables, rec.root_kind, rec.root_id, &m, error)) return false;
      obj.kind = ScriptSceneObjectKind::Manifold;
      obj.manifold = std::move(m);
      obj.mesh = obj.manifold.GetMeshGL();
      if (!compute_bounds(obj.mesh, &obj.bmin, &obj.bmax)) {
        return set_err(error, "Failed to compute bounds for scene object " + std::to_string(i));
      }
    } else if (rec.root_kind == (uint32_t)NodeKind::CrossSection) {
      manifold::CrossSection cs;
      if (!ResolveReplayCrossSection(tables, rec.root_kind, rec.root_id, &cs, error)) return false;
      obj.kind = ScriptSceneObjectKind::CrossSection;
      obj.mesh.numProp = 3;
      manifold::Polygons polys = cs.ToPolygons();
      obj.sketchContours.reserve(polys.size());
      for (const manifold::SimplePolygon &poly : polys) {
        if (poly.empty()) continue;
        ScriptSketchContour contour;
        contour.points.reserve(poly.size());
        for (const manifold::vec2 &p : poly) {
          contour.points.push_back({(float)p.x, (float)p.y, 0.0f});
        }
        obj.sketchContours.push_back(std::move(contour));
      }
      if (!compute_sketch_bounds(obj.sketchContours, &obj.bmin, &obj.bmax)) {
        obj.bmin = {0.0f, 0.0f, 0.0f};
        obj.bmax = {0.0f, 0.0f, 0.0f};
      }
    } else {
      return set_err(error, "Worker scene object has unsupported root kind.");
    }

    name_off += rec.name_len;
    objects->push_back(std::move(obj));
  }

  return true;
}

void ScriptWorkerClient::Shutdown() {
  if (conn_fd_ >= 0) {
    std::string unused;
    SendLine("SHUTDOWN\n", &unused);
    close(conn_fd_);
    conn_fd_ = -1;
  }
  if (listen_fd_ >= 0) {
    close(listen_fd_);
    listen_fd_ = -1;
  }
  if (!socket_path_.empty()) {
    unlink(socket_path_.c_str());
    socket_path_.clear();
  }
  if (worker_pid_ > 0) {
    kill(worker_pid_, SIGTERM);
    int status = 0;
    waitpid(worker_pid_, &status, 0);
    worker_pid_ = -1;
  }
  if (shm_ptr_) {
    munmap(shm_ptr_, shm_size_);
    shm_ptr_ = nullptr;
  }
  if (shm_fd_ >= 0) {
    close(shm_fd_);
    shm_fd_ = -1;
  }
  if (!shm_name_.empty()) {
    shm_unlink(shm_name_.c_str());
    shm_name_.clear();
  }
  started_ = false;
}

}  // namespace vicad
