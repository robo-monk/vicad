#include "edge_detection.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>

namespace vicad {

namespace {

struct Vec3d {
    double x;
    double y;
    double z;
};

struct AdjTri {
    int tri = -1;
};

struct ChainExtraction {
    std::vector<std::vector<int>> chains;
    std::vector<uint8_t> keptEdgeMask;
};

static Vec3d add(const Vec3d &a, const Vec3d &b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

static Vec3d sub(const Vec3d &a, const Vec3d &b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

static Vec3d mul(const Vec3d &v, double s) {
    return {v.x * s, v.y * s, v.z * s};
}

static Vec3d cross(const Vec3d &a, const Vec3d &b) {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}

static double dot(const Vec3d &a, const Vec3d &b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static double length(const Vec3d &v) {
    return std::sqrt(dot(v, v));
}

static Vec3d normalize(const Vec3d &v) {
    const double l = length(v);
    if (l <= 1e-20) return {0.0, 0.0, 0.0};
    return {v.x / l, v.y / l, v.z / l};
}

static Vec3d mesh_pos(const manifold::MeshGL &mesh, uint32_t idx) {
    const size_t base = (size_t)idx * (size_t)mesh.numProp;
    return {
        (double)mesh.vertProperties[base + 0],
        (double)mesh.vertProperties[base + 1],
        (double)mesh.vertProperties[base + 2],
    };
}

static uint64_t edge_key(uint32_t a, uint32_t b) {
    if (a > b) std::swap(a, b);
    return ((uint64_t)a << 32) | (uint64_t)b;
}

static bool point_ray_distance(const Vec3d &p,
                               const Vec3d &rayOrig,
                               const Vec3d &rayDir,
                               double *outT,
                               double *outDist) {
    const Vec3d op = sub(p, rayOrig);
    const double t = dot(op, rayDir);
    if (!std::isfinite(t) || t <= 1e-9) return false;
    const Vec3d q = add(rayOrig, mul(rayDir, t));
    const double d = length(sub(p, q));
    if (!std::isfinite(d)) return false;
    if (outT) *outT = t;
    if (outDist) *outDist = d;
    return true;
}

static uint32_t other_vertex(const EdgeRecord &e, uint32_t v) {
    return (e.v0 == v) ? e.v1 : e.v0;
}

static Vec3d edge_dir_from_vertex(const manifold::MeshGL &mesh,
                                  const EdgeRecord &e,
                                  uint32_t fromV) {
    const Vec3d p0 = mesh_pos(mesh, fromV);
    const Vec3d p1 = mesh_pos(mesh, other_vertex(e, fromV));
    return normalize(sub(p1, p0));
}

static ChainExtraction extract_chains(const manifold::MeshGL &mesh,
                                      const std::vector<EdgeRecord> &edges,
                                      const std::vector<uint8_t> &includeMask,
                                      double maxTurnDeg,
                                      const std::vector<double> &edgeLengths,
                                      double minChainLength,
                                      int minSegments,
                                      const std::vector<uint8_t> *preserveMask) {
    ChainExtraction out = {};
    out.keptEdgeMask.assign(edges.size(), 0);
    if (edges.empty() || includeMask.size() != edges.size()) return out;

    const size_t numVerts = mesh.numProp >= 3 ? mesh.vertProperties.size() / (size_t)mesh.numProp : 0;
    if (numVerts == 0) return out;

    std::vector<std::vector<int>> incident(numVerts);
    std::vector<int> degree(numVerts, 0);

    for (size_t i = 0; i < edges.size(); ++i) {
        if (includeMask[i] == 0) continue;
        const EdgeRecord &e = edges[i];
        if ((size_t)e.v0 >= numVerts || (size_t)e.v1 >= numVerts) continue;
        incident[e.v0].push_back((int)i);
        incident[e.v1].push_back((int)i);
        degree[e.v0] += 1;
        degree[e.v1] += 1;
    }

    std::vector<uint8_t> visited(edges.size(), 0);

    constexpr double kPi = 3.14159265358979323846;
    const double minCos = std::cos(maxTurnDeg * kPi / 180.0);

    auto choose_next = [&](int curEdge, uint32_t atVertex, const Vec3d &incoming) {
        int best = -1;
        double bestScore = -2.0;
        if ((size_t)atVertex >= incident.size()) return -1;
        for (int cand : incident[atVertex]) {
            if (cand == curEdge) continue;
            if (cand < 0 || (size_t)cand >= edges.size()) continue;
            if (includeMask[(size_t)cand] == 0 || visited[(size_t)cand] != 0) continue;
            const Vec3d outDir = edge_dir_from_vertex(mesh, edges[(size_t)cand], atVertex);
            const double score = dot(incoming, outDir);
            if (score > bestScore) {
                bestScore = score;
                best = cand;
            }
        }
        if (best < 0) return -1;
        if (bestScore < minCos) return -1;
        return best;
    };

    auto trace_chain = [&](int startEdge, uint32_t startVertex) {
        std::vector<int> chain;
        int cur = startEdge;
        uint32_t fromV = startVertex;

        while (cur >= 0 && (size_t)cur < edges.size()) {
            if (visited[(size_t)cur] != 0) break;
            visited[(size_t)cur] = 1;
            chain.push_back(cur);

            const EdgeRecord &e = edges[(size_t)cur];
            const uint32_t toV = other_vertex(e, fromV);
            const Vec3d incoming = edge_dir_from_vertex(mesh, e, fromV);
            const int next = choose_next(cur, toV, incoming);

            fromV = toV;
            cur = next;
        }

        return chain;
    };

    for (size_t i = 0; i < edges.size(); ++i) {
        if (includeMask[i] == 0 || visited[i] != 0) continue;
        const EdgeRecord &e = edges[i];
        const bool end0 = (size_t)e.v0 < degree.size() ? degree[e.v0] != 2 : true;
        const bool end1 = (size_t)e.v1 < degree.size() ? degree[e.v1] != 2 : true;
        if (!end0 && !end1) continue;
        const uint32_t startV = end0 ? e.v0 : e.v1;
        std::vector<int> chain = trace_chain((int)i, startV);
        if (!chain.empty()) out.chains.push_back(std::move(chain));
    }

    for (size_t i = 0; i < edges.size(); ++i) {
        if (includeMask[i] == 0 || visited[i] != 0) continue;
        std::vector<int> chain = trace_chain((int)i, edges[i].v0);
        if (!chain.empty()) out.chains.push_back(std::move(chain));
    }

    double longestLen = -1.0;
    int longestIdx = -1;

    for (size_t ci = 0; ci < out.chains.size(); ++ci) {
        const std::vector<int> &chain = out.chains[ci];
        double chainLen = 0.0;
        bool preserve = false;
        for (const int ei : chain) {
            if (ei < 0 || (size_t)ei >= edgeLengths.size()) continue;
            chainLen += edgeLengths[(size_t)ei];
            if (preserveMask && (size_t)ei < preserveMask->size() && (*preserveMask)[(size_t)ei] != 0) {
                preserve = true;
            }
        }
        if (chainLen > longestLen) {
            longestLen = chainLen;
            longestIdx = (int)ci;
        }

        if ((int)chain.size() < minSegments && !preserve) continue;
        if (chainLen < minChainLength && !preserve) continue;

        for (const int ei : chain) {
            if (ei >= 0 && (size_t)ei < out.keptEdgeMask.size()) out.keptEdgeMask[(size_t)ei] = 1;
        }
    }

    bool anyKept = false;
    for (const uint8_t v : out.keptEdgeMask) {
        if (v != 0) {
            anyKept = true;
            break;
        }
    }

    if (!anyKept && longestIdx >= 0) {
        for (const int ei : out.chains[(size_t)longestIdx]) {
            if (ei >= 0 && (size_t)ei < out.keptEdgeMask.size()) out.keptEdgeMask[(size_t)ei] = 1;
        }
    }

    return out;
}

}  // namespace

EdgeDetectionResult BuildEdgeTopology(const manifold::MeshGL &mesh, float sharpAngleDeg) {
    EdgeDetectionResult out = {};
    const uint32_t triCount = (uint32_t)mesh.NumTri();
    if (triCount == 0 || mesh.numProp < 3) return out;

    std::vector<Vec3d> triNormal(triCount);
    Vec3d mn = mesh_pos(mesh, mesh.triVerts[0]);
    Vec3d mx = mn;

    for (uint32_t tri = 0; tri < triCount; ++tri) {
        const uint32_t i0 = mesh.triVerts[tri * 3 + 0];
        const uint32_t i1 = mesh.triVerts[tri * 3 + 1];
        const uint32_t i2 = mesh.triVerts[tri * 3 + 2];
        const Vec3d p0 = mesh_pos(mesh, i0);
        const Vec3d p1 = mesh_pos(mesh, i1);
        const Vec3d p2 = mesh_pos(mesh, i2);
        triNormal[tri] = normalize(cross(sub(p1, p0), sub(p2, p0)));

        mn.x = std::min({mn.x, p0.x, p1.x, p2.x});
        mn.y = std::min({mn.y, p0.y, p1.y, p2.y});
        mn.z = std::min({mn.z, p0.z, p1.z, p2.z});
        mx.x = std::max({mx.x, p0.x, p1.x, p2.x});
        mx.y = std::max({mx.y, p0.y, p1.y, p2.y});
        mx.z = std::max({mx.z, p0.z, p1.z, p2.z});
    }

    const double bboxDiag = std::max(length(sub(mx, mn)), 1e-6);

    std::unordered_map<uint64_t, std::vector<AdjTri>> edgeToTris;
    edgeToTris.reserve((size_t)triCount * 2);

    for (uint32_t tri = 0; tri < triCount; ++tri) {
        const uint32_t i0 = mesh.triVerts[tri * 3 + 0];
        const uint32_t i1 = mesh.triVerts[tri * 3 + 1];
        const uint32_t i2 = mesh.triVerts[tri * 3 + 2];

        edgeToTris[edge_key(i0, i1)].push_back({(int)tri});
        edgeToTris[edge_key(i1, i2)].push_back({(int)tri});
        edgeToTris[edge_key(i2, i0)].push_back({(int)tri});
    }

    out.edges.reserve(edgeToTris.size());
    out.edgeFlags.reserve(edgeToTris.size());

    std::vector<double> edgeLengths;
    edgeLengths.reserve(edgeToTris.size());

    constexpr double kPi = 3.14159265358979323846;
    const double sharpCos = std::cos((double)sharpAngleDeg * kPi / 180.0);

    for (const auto &it : edgeToTris) {
        const uint64_t key = it.first;
        const uint32_t v0 = (uint32_t)(key >> 32);
        const uint32_t v1 = (uint32_t)(key & 0xffffffffu);
        const std::vector<AdjTri> &tris = it.second;

        EdgeRecord rec = {};
        rec.v0 = v0;
        rec.v1 = v1;
        if (!tris.empty()) {
            rec.triA = tris[0].tri;
            rec.nA = {(double)triNormal[(size_t)tris[0].tri].x,
                      (double)triNormal[(size_t)tris[0].tri].y,
                      (double)triNormal[(size_t)tris[0].tri].z};
        }
        if (tris.size() >= 2) {
            rec.triB = tris[1].tri;
            rec.nB = {(double)triNormal[(size_t)tris[1].tri].x,
                      (double)triNormal[(size_t)tris[1].tri].y,
                      (double)triNormal[(size_t)tris[1].tri].z};
        }

        uint8_t flags = EdgeClassNone;
        if (tris.size() == 1) {
            flags |= EdgeClassBoundary;
        } else if (tris.size() != 2) {
            flags |= EdgeClassNonManifold;
        } else {
            const Vec3d nA = triNormal[(size_t)tris[0].tri];
            const Vec3d nB = triNormal[(size_t)tris[1].tri];
            const double d = dot(nA, nB);
            if (std::isfinite(d) && d < sharpCos) {
                flags |= EdgeClassSharp;
            }
        }

        out.edges.push_back(rec);
        out.edgeFlags.push_back(flags);

        const Vec3d p0 = mesh_pos(mesh, v0);
        const Vec3d p1 = mesh_pos(mesh, v1);
        edgeLengths.push_back(length(sub(p1, p0)));
    }

    std::vector<double> sortedLengths = edgeLengths;
    std::sort(sortedLengths.begin(), sortedLengths.end());
    const double medianLen = sortedLengths.empty() ? 0.0 : sortedLengths[sortedLengths.size() / 2];
    const double minSharpLen = std::max(1e-8, medianLen * 0.25);

    std::vector<uint8_t> featureMask(out.edges.size(), 0);
    std::vector<uint8_t> preserveMask(out.edges.size(), 0);
    for (size_t i = 0; i < out.edges.size(); ++i) {
        const uint8_t flags = out.edgeFlags[i];
        const bool boundary = (flags & EdgeClassBoundary) != 0;
        const bool nonManifold = (flags & EdgeClassNonManifold) != 0;
        const bool sharp = (flags & EdgeClassSharp) != 0;

        if (boundary || nonManifold) {
            featureMask[i] = 1;
            preserveMask[i] = 1;
            continue;
        }
        if (sharp && edgeLengths[i] >= minSharpLen) {
            featureMask[i] = 1;
        }
    }

    const double minChainLen = std::max(1e-4, bboxDiag * 0.015);
    ChainExtraction extraction = extract_chains(
        mesh,
        out.edges,
        featureMask,
        35.0,
        edgeLengths,
        minChainLen,
        2,
        &preserveMask);

    out.sharpEdgeIndices.clear();
    out.boundaryEdgeIndices.clear();
    out.nonManifoldEdgeIndices.clear();
    out.featureChains.clear();
    out.edgeFeatureChain.assign(out.edges.size(), -1);

    for (size_t i = 0; i < out.edges.size(); ++i) {
        if (i >= extraction.keptEdgeMask.size() || extraction.keptEdgeMask[i] == 0) continue;
        const uint8_t flags = out.edgeFlags[i];
        if ((flags & EdgeClassSharp) != 0) out.sharpEdgeIndices.push_back((int)i);
        if ((flags & EdgeClassBoundary) != 0) out.boundaryEdgeIndices.push_back((int)i);
        if ((flags & EdgeClassNonManifold) != 0) out.nonManifoldEdgeIndices.push_back((int)i);
    }

    for (const std::vector<int> &chain : extraction.chains) {
        std::vector<int> kept;
        kept.reserve(chain.size());
        for (const int ei : chain) {
            if (ei < 0 || (size_t)ei >= extraction.keptEdgeMask.size()) continue;
            if (extraction.keptEdgeMask[(size_t)ei] == 0) continue;
            kept.push_back(ei);
        }
        if (kept.empty()) continue;
        const int chainId = (int)out.featureChains.size();
        for (const int ei : kept) {
            if (ei >= 0 && (size_t)ei < out.edgeFeatureChain.size()) {
                out.edgeFeatureChain[(size_t)ei] = chainId;
            }
        }
        out.featureChains.push_back(std::move(kept));
    }

    return out;
}

SilhouetteResult ComputeSilhouetteEdges(const manifold::MeshGL &mesh,
                                        const EdgeDetectionResult &edges,
                                        double eyeX, double eyeY, double eyeZ) {
    SilhouetteResult out = {};
    out.isSilhouette.assign(edges.edges.size(), 0);
    if (mesh.numProp < 3 || edges.edges.empty()) return out;

    const Vec3d eye = {eyeX, eyeY, eyeZ};

    Vec3d mn = mesh_pos(mesh, edges.edges[0].v0);
    Vec3d mx = mn;
    std::vector<double> edgeLengths(edges.edges.size(), 0.0);

    for (size_t i = 0; i < edges.edges.size(); ++i) {
        const EdgeRecord &edge = edges.edges[i];
        const Vec3d p0 = mesh_pos(mesh, edge.v0);
        const Vec3d p1 = mesh_pos(mesh, edge.v1);
        edgeLengths[i] = length(sub(p1, p0));
        mn.x = std::min({mn.x, p0.x, p1.x});
        mn.y = std::min({mn.y, p0.y, p1.y});
        mn.z = std::min({mn.z, p0.z, p1.z});
        mx.x = std::max({mx.x, p0.x, p1.x});
        mx.y = std::max({mx.y, p0.y, p1.y});
        mx.z = std::max({mx.z, p0.z, p1.z});
    }

    const double bboxDiag = std::max(length(sub(mx, mn)), 1e-6);
    std::vector<uint8_t> silhouetteMask(edges.edges.size(), 0);

    for (size_t i = 0; i < edges.edges.size(); ++i) {
        const uint8_t flags = (i < edges.edgeFlags.size()) ? edges.edgeFlags[i] : 0;
        if ((flags & EdgeClassNonManifold) != 0) continue;

        const EdgeRecord &e = edges.edges[i];
        if (e.triA < 0 || e.triB < 0) continue;

        const Vec3d p0 = mesh_pos(mesh, e.v0);
        const Vec3d p1 = mesh_pos(mesh, e.v1);
        const Vec3d mid = mul(add(p0, p1), 0.5);
        const Vec3d viewDir = normalize(sub(eye, mid));

        const Vec3d nA = {e.nA.x, e.nA.y, e.nA.z};
        const Vec3d nB = {e.nB.x, e.nB.y, e.nB.z};
        const double da = dot(nA, viewDir);
        const double db = dot(nB, viewDir);
        if (!std::isfinite(da) || !std::isfinite(db)) continue;

        if ((da > 0.0 && db <= 0.0) || (da <= 0.0 && db > 0.0)) {
            silhouetteMask[i] = 1;
        }
    }

    ChainExtraction extraction = extract_chains(
        mesh,
        edges.edges,
        silhouetteMask,
        42.0,
        edgeLengths,
        std::max(1e-4, bboxDiag * 0.02),
        3,
        nullptr);

    for (size_t i = 0; i < extraction.keptEdgeMask.size(); ++i) {
        if (extraction.keptEdgeMask[i] == 0) continue;
        out.isSilhouette[i] = 1;
        out.silhouetteEdgeIndices.push_back((int)i);
    }

    return out;
}

int PickEdgeByRay(const manifold::MeshGL &mesh,
                  const EdgeDetectionResult &edges,
                  const SilhouetteResult &silhouette,
                  double rayOriginX, double rayOriginY, double rayOriginZ,
                  double rayDirX, double rayDirY, double rayDirZ,
                  double pickRadius,
                  double *outDistance) {
    if (mesh.numProp < 3 || edges.edges.empty()) return -1;

    const Vec3d rayOrig = {rayOriginX, rayOriginY, rayOriginZ};
    const Vec3d rayDir = normalize({rayDirX, rayDirY, rayDirZ});
    if (length(rayDir) <= 1e-20) return -1;

    std::vector<uint8_t> candidate(edges.edges.size(), 0);
    for (int idx : edges.sharpEdgeIndices) {
        if (idx >= 0 && (size_t)idx < candidate.size()) candidate[(size_t)idx] = 1;
    }
    for (int idx : edges.boundaryEdgeIndices) {
        if (idx >= 0 && (size_t)idx < candidate.size()) candidate[(size_t)idx] = 1;
    }
    for (int idx : edges.nonManifoldEdgeIndices) {
        if (idx >= 0 && (size_t)idx < candidate.size()) candidate[(size_t)idx] = 1;
    }
    for (int idx : silhouette.silhouetteEdgeIndices) {
        if (idx >= 0 && (size_t)idx < candidate.size()) candidate[(size_t)idx] = 1;
    }

    double bestT = std::numeric_limits<double>::infinity();
    double bestDist = std::numeric_limits<double>::infinity();
    int bestEdge = -1;

    for (size_t i = 0; i < edges.edges.size(); ++i) {
        if (candidate[i] == 0) continue;
        const EdgeRecord &e = edges.edges[i];

        const Vec3d p0 = mesh_pos(mesh, e.v0);
        const Vec3d p1 = mesh_pos(mesh, e.v1);

        double t0 = 0.0;
        double d0 = 0.0;
        double t1 = 0.0;
        double d1 = 0.0;
        const bool ok0 = point_ray_distance(p0, rayOrig, rayDir, &t0, &d0);
        const bool ok1 = point_ray_distance(p1, rayOrig, rayDir, &t1, &d1);
        if (!ok0 && !ok1) continue;

        double t = ok0 ? t0 : t1;
        double d = ok0 ? d0 : d1;
        if (ok0 && ok1) {
            t = std::min(t0, t1);
            d = std::min(d0, d1);
        }

        const Vec3d mid = mul(add(p0, p1), 0.5);
        double tm = 0.0;
        double dm = 0.0;
        if (point_ray_distance(mid, rayOrig, rayDir, &tm, &dm)) {
            if (tm < t) t = tm;
            if (dm < d) d = dm;
        }

        if (d > pickRadius) continue;
        if (t < bestT || (std::fabs(t - bestT) <= 1e-9 && d < bestDist)) {
            bestT = t;
            bestDist = d;
            bestEdge = (int)i;
        }
    }

    if (bestEdge >= 0 && outDistance) *outDistance = bestT;
    return bestEdge;
}

}  // namespace vicad
