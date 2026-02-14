#include "script_worker_client.h"

#include <cerrno>
#include <csignal>
#include <chrono>
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

const char *phase_name(uint32_t phase) {
  switch ((IpcErrorPhase)phase) {
    case IpcErrorPhase::RequestDecode: return "request_decode";
    case IpcErrorPhase::ScriptLoad: return "script_load";
    case IpcErrorPhase::ScriptExecute: return "script_execute";
    case IpcErrorPhase::SceneEncode: return "scene_encode";
    case IpcErrorPhase::ResponseDecode: return "response_decode";
    case IpcErrorPhase::Transport: return "transport";
    case IpcErrorPhase::Unknown:
    default: return "unknown";
  }
}

std::string format_diagnostic_message(const ScriptExecutionDiagnostic &diag) {
  std::string out = "phase=" + std::string(phase_name(diag.phase));
  if (!diag.file.empty()) {
    out += " file=" + diag.file;
    if (diag.line > 0) {
      out += ":" + std::to_string(diag.line);
      if (diag.column > 0) out += ":" + std::to_string(diag.column);
    }
  }
  if (diag.durationMs > 0) out += " duration_ms=" + std::to_string(diag.durationMs);
  if (!diag.message.empty()) out += "\n" + diag.message;
  if (!diag.stack.empty()) out += "\n" + diag.stack;
  return out;
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

bool read_error_message(const SharedHeader *hdr, const uint8_t *base,
                        ScriptExecutionDiagnostic *diag, std::string *error) {
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
  const size_t total = sizeof(resp) + (size_t)resp.file_len + (size_t)resp.stack_len + (size_t)resp.message_len;
  if (total > hdr->response_length) return set_err(error, "Worker error message is truncated.");
  const uint8_t *payload = payload_ptr + sizeof(resp);
  diag->errorCode = resp.error_code;
  diag->phase = resp.phase;
  diag->line = resp.line;
  diag->column = resp.column;
  diag->runId = resp.run_id;
  diag->durationMs = resp.duration_ms;
  diag->file.assign((const char *)payload, (size_t)resp.file_len);
  payload += resp.file_len;
  diag->stack.assign((const char *)payload, (size_t)resp.stack_len);
  payload += resp.stack_len;
  diag->message.assign((const char *)payload, (size_t)resp.message_len);
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
      next_seq_(1),
      last_diagnostic_() {}

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
  LogEvent("WORKER_STARTING", 0);
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
  LogEvent("WORKER_STARTED", 0, "pid=" + std::to_string(pid));
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
  LogEvent("WORKER_CONNECTED", 0);
  return true;
}

bool ScriptWorkerClient::Start(std::string *error) {
  if (started_) return true;
  if (!CreateSharedMemory(error)) {
    Shutdown();
    return false;
  }
  if (!CreateSocket(error)) {
    Shutdown();
    return false;
  }
  if (!SpawnWorker(error)) {
    Shutdown();
    return false;
  }
  if (!AcceptWorker(error)) {
    Shutdown();
    return false;
  }
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

bool ScriptWorkerClient::ExecuteScriptScene(const char *script_path,
                                            std::vector<ScriptSceneObject> *objects,
                                            std::string *error,
                                            const ReplayLodPolicy &lod_policy) {
  if (!Start(error)) return false;
  if (!script_path || !objects) return set_err(error, "Invalid execute arguments.");
  objects->clear();
  last_diagnostic_ = {};

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
  const auto run_started = std::chrono::steady_clock::now();
  hdr->request_seq = seq;
  hdr->request_length = (uint32_t)req_size;
  hdr->response_length = 0;
  hdr->error_code = (uint32_t)IpcErrorCode::None;
  hdr->state = (uint32_t)IpcState::RequestReady;

  LogEvent("RUN_QUEUED", seq, script_path);
  if (!SendLine("RUN " + std::to_string(seq) + "\n", error)) {
    Shutdown();
    return false;
  }
  LogEvent("RUN_STARTED", seq);

  std::string line;
  if (!ReadLineWithTimeout(30 * 1000, &line, error)) {
    LogEvent("RUN_FAILED", seq, "transport_timeout");
    Shutdown();
    return false;
  }

  const std::string done = "DONE " + std::to_string(seq);
  const std::string fail = "ERROR " + std::to_string(seq);
  if (line == fail) {
    std::string read_err;
    if (!read_error_message(hdr, base, &last_diagnostic_, &read_err)) return set_err(error, read_err);
    if (last_diagnostic_.durationMs == 0) {
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - run_started).count();
      last_diagnostic_.durationMs = (uint32_t)((ms < 0) ? 0 : ms);
    }
    LogEvent("RUN_FAILED", seq, "phase=" + std::string(phase_name(last_diagnostic_.phase)));
    return set_err(error, format_diagnostic_message(last_diagnostic_));
  }
  if (line != done) {
    LogEvent("RUN_FAILED", seq, "unexpected_response");
    Shutdown();
    return set_err(error, "Unexpected worker response: " + line);
  }
  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - run_started).count();
  LogEvent("RUN_DONE", seq, "duration_ms=" + std::to_string((long long)elapsed_ms));

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
  if (!ReplayOpsToTables(records_ptr, ok.records_size, ok.op_count,
                         lod_policy, &tables, error)) {
    return false;
  }

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
    obj.rootKind = rec.root_kind;
    obj.rootId = rec.root_id;

    std::string trace_error;
    if (!BuildOperationTraceForRoot(tables, rec.root_kind, rec.root_id, &obj.opTrace, &trace_error)) {
      return set_err(error, trace_error);
    }

    if (rec.root_kind == (uint32_t)NodeKind::Manifold) {
      manifold::Manifold m;
      if (!ResolveReplayManifold(tables, rec.root_kind, rec.root_id,
                                 lod_policy, &m, error)) {
        return false;
      }
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
      SketchDimensionModel dims;
      std::string dim_error;
      if (BuildSketchDimensionModelForRoot(tables, rec.root_id, &dims, &dim_error)) {
        obj.sketchDims = std::move(dims);
      } else {
        obj.sketchDims.reset();
      }
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

void ScriptWorkerClient::LogEvent(const char *event, uint64_t run_id, const std::string &details) {
  if (!event) return;
  if (details.empty()) {
    std::fprintf(stderr, "[vicad-ipc] %s run_id=%llu\n",
                 event, (unsigned long long)run_id);
  } else {
    std::fprintf(stderr, "[vicad-ipc] %s run_id=%llu %s\n",
                 event, (unsigned long long)run_id, details.c_str());
  }
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
    LogEvent("WORKER_STOPPING", 0, "pid=" + std::to_string(worker_pid_));
    kill(worker_pid_, SIGTERM);
    int status = 0;
    waitpid(worker_pid_, &status, 0);
    LogEvent("WORKER_STOPPED", 0, "status=" + std::to_string(status));
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
