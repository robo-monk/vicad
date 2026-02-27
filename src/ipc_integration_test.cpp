// ipc_integration_test.cpp
//
// End-to-end IPC integration test.
//
// Exercises the full path:
//   .vicad.ts script
//     → bun worker (socket + shm)
//       → op stream in shared memory
//         → ScriptWorkerClient decodes
//           → ScriptSceneObject list
//
// Must be run from the repo root (where sketch-fillet-example.vicad.ts lives)
// with `bun` on PATH.

#include <cmath>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "ipc_protocol.h"
#include "lod_policy.h"
#include "script_worker_client.h"

namespace {

static int g_pass = 0;
static int g_fail = 0;

bool require(bool cond, const char *label) {
  if (cond) {
    std::cout << "  PASS: " << label << "\n";
    ++g_pass;
  } else {
    std::cout << "  FAIL: " << label << "\n";
    ++g_fail;
  }
  return cond;
}

// ── Test: sketch-fillet-example.vicad.ts ─────────────────────────────────────
//
// The script adds two scene objects:
//   [0] "Per-Corner Fillet Profile"  — CrossSection (2D sketch)
//   [1] "Per-Corner Fillet Plate"    — Manifold (extruded solid, 80×50×8)
bool test_fillet_example() {
  std::cout << "\n[ipc_integration_test] sketch-fillet-example.vicad.ts\n";

  vicad::ScriptWorkerClient client;
  std::vector<vicad::ScriptSceneObject> objects;
  std::string error;
  vicad::ReplayLodPolicy lod = {};
  lod.profile = vicad::LodProfile::Model;

  const bool ok = client.ExecuteScriptScene(
      "sketch-fillet-example.vicad.ts", &objects, &error, lod);

  if (!require(ok, "ExecuteScriptScene returned true")) {
    std::cout << "  error: " << error << "\n";
    return false;
  }

  if (!require(objects.size() == 2, "scene has exactly 2 objects")) return false;

  // Object 0: the 2D fillet profile
  require(objects[0].name == "Per-Corner Fillet Profile",
          "objects[0] name is 'Per-Corner Fillet Profile'");
  require(objects[0].kind == vicad::ScriptSceneObjectKind::CrossSection,
          "objects[0] kind is CrossSection");
  require(!objects[0].sketchContours.empty(),
          "objects[0] has sketch contours");

  // Object 1: the extruded solid
  require(objects[1].name == "Per-Corner Fillet Plate",
          "objects[1] name is 'Per-Corner Fillet Plate'");
  require(objects[1].kind == vicad::ScriptSceneObjectKind::Manifold,
          "objects[1] kind is Manifold");
  require(!objects[1].mesh.vertProperties.empty(),
          "objects[1] mesh has vertices");

  // Bounds check: extrusion of 80×50 profile by height 8.
  // Allow 1 mm tolerance for fillet rounding effects on the extents.
  const vicad::SceneVec3 bmin = objects[1].bmin;
  const vicad::SceneVec3 bmax = objects[1].bmax;
  const float dx = bmax.x - bmin.x;
  const float dy = bmax.y - bmin.y;
  const float dz = bmax.z - bmin.z;

  require(std::fabs(dx - 80.0f) < 1.0f, "objects[1] X extent ≈ 80");
  require(std::fabs(dy - 50.0f) < 1.0f, "objects[1] Y extent ≈ 50");
  require(std::fabs(dz - 8.0f) < 0.1f,  "objects[1] Z extent ≈ 8");

  return g_fail == 0;
}

}  // namespace

int main() {
  std::cout << "[ipc_integration_test] starting\n";

  const bool all_passed = test_fillet_example();

  std::cout << "\n[ipc_integration_test] "
            << g_pass << " passed, " << g_fail << " failed\n";

  if (all_passed) {
    std::cout << "[ipc_integration_test] PASS\n";
    return 0;
  }
  std::cout << "[ipc_integration_test] FAIL\n";
  return 1;
}
