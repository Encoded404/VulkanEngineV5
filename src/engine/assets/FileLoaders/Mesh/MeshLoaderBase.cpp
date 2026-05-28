module;

#include <vector>
#include <array>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <limits>

module VulkanEngine.FileLoaders.Mesh.MeshLoaderBase;

import VulkanEngine.Mesh.MeshTypes;

namespace VulkanEngine::FileLoaders::Mesh {

namespace {

void ComputeSubmeshBoundingVolumes(const std::vector<MeshVertexVec3>& vertices,
                                    const std::vector<uint32_t>& indices,
                                    SubMesh& sm) {
    // Gather positions for this submesh
    std::vector<MeshVertexVec3> positions;
    positions.reserve(sm.index_count);
    for (uint32_t i = sm.index_start; i < sm.index_start + sm.index_count; ++i) {
        positions.push_back(vertices[indices[i]]);
    }

    if (positions.empty()) return;

    // ── Bounding sphere (Ritter's algorithm) ──
    auto dist2 = [](const MeshVertexVec3& a, const MeshVertexVec3& b) {
        const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        return dx*dx + dy*dy + dz*dz;
    };

    MeshVertexVec3 p1 = positions[0];
    MeshVertexVec3 p2 = positions[0];
    for (const auto& v : positions) {
        if (dist2(v, p1) > dist2(p2, p1)) p2 = v;
    }
    for (const auto& v : positions) {
        if (dist2(v, p2) > dist2(p1, p2)) p1 = v;
    }

    MeshVertexVec3 center;
    center.x = (p1.x + p2.x) * 0.5f;
    center.y = (p1.y + p2.y) * 0.5f;
    center.z = (p1.z + p2.z) * 0.5f;
    float radius = std::sqrt(dist2(p1, center));

    for (const auto& v : positions) {
        const float d = std::sqrt(dist2(v, center));
        if (d > radius) {
            const float t = (d - radius) / (2.0f * d);
            center.x += (v.x - center.x) * t;
            center.y += (v.y - center.y) * t;
            center.z += (v.z - center.z) * t;
            radius = (radius + d) * 0.5f;
        }
    }

    sm.sphere.center = center;
    sm.sphere.radius = radius;

    // ── OBB centroid ──
    MeshVertexVec3 obb_center{};
    for (const auto& v : positions) {
        obb_center.x += v.x;
        obb_center.y += v.y;
        obb_center.z += v.z;
    }
    obb_center.x /= static_cast<float>(positions.size());
    obb_center.y /= static_cast<float>(positions.size());
    obb_center.z /= static_cast<float>(positions.size());

    // ── Covariance matrix (3x3 symmetric, 6 unique values) ──
    float cxx = 0, cxy = 0, cxz = 0;
    float cyy = 0, cyz = 0, czz = 0;
    for (const auto& v : positions) {
        const float dx = v.x - obb_center.x;
        const float dy = v.y - obb_center.y;
        const float dz = v.z - obb_center.z;
        cxx += dx * dx; cxy += dx * dy; cxz += dx * dz;
        cyy += dy * dy; cyz += dy * dz;
        czz += dz * dz;
    }
    const float n = static_cast<float>(positions.size());
    cxx /= n; cxy /= n; cxz /= n;
    cyy /= n; cyz /= n;
    czz /= n;

    // ── Jacobi iteration (10 iterations for 3x3) ──
    // Matrix A = [[cxx, cxy, cxz], [cxy, cyy, cyz], [cxz, cyz, czz]]
    float a00 = cxx, a01 = cxy, a02 = cxz;
    float a11 = cyy, a12 = cyz;
    float a22 = czz;

    // Eigenvectors V = identity
    float v00 = 1, v01 = 0, v02 = 0;
    float v10 = 0, v11 = 1, v12 = 0;
    float v20 = 0, v21 = 0, v22 = 1;

    for (int iter = 0; iter < 10; ++iter) {
        // Find largest off-diagonal
        const float max_off = std::max({std::abs(a01), std::abs(a02), std::abs(a12)});
        if (max_off < 1e-10f) break;

        int p = 0, q = 0;
        if (std::abs(a01) >= max_off) { p = 0; q = 1; }
        else if (std::abs(a02) >= max_off) { p = 0; q = 2; }
        else { p = 1; q = 2; }

        const float theta = 0.5f * std::atan2(2.0f * (p == 0 && q == 1 ? a01 : p == 0 && q == 2 ? a02 : a12),
                                                (p == 0 ? (q == 1 ? a00 - a11 : a00 - a22) : a11 - a22));
        const float c = std::cos(theta);
        const float s = std::sin(theta);

        // A = J^T * A * J (only touches rows/cols p,q)
        if (p == 0 && q == 1) {
            const float app = a00, aqq = a11, apq = a01;
            const float new_a00 = c*c*app + s*s*aqq - 2*s*c*apq;
            const float new_a11 = s*s*app + c*c*aqq + 2*s*c*apq;
            const float new_a01 = (c*c - s*s)*apq + s*c*(app - aqq);
            const float a0p = a02, a1q = a12;
            a00 = new_a00; a11 = new_a11; a01 = new_a01;
            a02 = c*a0p - s*a1q;
            a12 = s*a0p + c*a1q;
        } else if (p == 0 && q == 2) {
            const float app = a00, aqq = a22, apq = a02;
            const float new_a00 = c*c*app + s*s*aqq - 2*s*c*apq;
            const float new_a22 = s*s*app + c*c*aqq + 2*s*c*apq;
            const float new_a02 = (c*c - s*s)*apq + s*c*(app - aqq);
            const float a0p = a01, a1q = a12;
            a00 = new_a00; a22 = new_a22; a02 = new_a02;
            a01 = c*a0p - s*a1q;
            a12 = s*a0p + c*a1q;
        } else { // p == 1, q == 2
            const float app = a11, aqq = a22, apq = a12;
            const float new_a11 = c*c*app + s*s*aqq - 2*s*c*apq;
            const float new_a22 = s*s*app + c*c*aqq + 2*s*c*apq;
            const float new_a12 = (c*c - s*s)*apq + s*c*(app - aqq);
            const float a0p = a01, a1q = a02;
            a11 = new_a11; a22 = new_a22; a12 = new_a12;
            a01 = c*a0p - s*a1q;
            a02 = s*a0p + c*a1q;
        }

        // V = V * J
        for (int row = 0; row < 3; ++row) {
            const float vi_p = (row == 0 ? (p == 0 ? v00 : p == 1 ? v01 : v02) :
                                row == 1 ? (p == 0 ? v10 : p == 1 ? v11 : v12) :
                                           (p == 0 ? v20 : p == 1 ? v21 : v22));
            const float vi_q = (row == 0 ? (q == 0 ? v00 : q == 1 ? v01 : v02) :
                                row == 1 ? (q == 0 ? v10 : q == 1 ? v11 : v12) :
                                           (q == 0 ? v20 : q == 1 ? v21 : v22));
            const float new_vip = c * vi_p - s * vi_q;
            const float new_viq = s * vi_p + c * vi_q;
            if (row == 0) {
                if (p == 0) v00 = new_vip; else if (p == 1) v01 = new_vip; else v02 = new_vip;
                if (q == 0) v00 = new_viq; else if (q == 1) v01 = new_viq; else v02 = new_viq;
            } else if (row == 1) {
                if (p == 0) v10 = new_vip; else if (p == 1) v11 = new_vip; else v12 = new_vip;
                if (q == 0) v10 = new_viq; else if (q == 1) v11 = new_viq; else v12 = new_viq;
            } else {
                if (p == 0) v20 = new_vip; else if (p == 1) v21 = new_vip; else v22 = new_vip;
                if (q == 0) v20 = new_viq; else if (q == 1) v21 = new_viq; else v22 = new_viq;
            }
        }
    }

    // Eigenvalues are on diagonal of A
    // Sort eigenvectors by eigenvalue (descending)
    struct EigenPair { float val; float x, y, z; };
    std::array<EigenPair, 3> ev = {{
        {a00, v00, v10, v20},
        {a11, v01, v11, v21},
        {a22, v02, v12, v22}
    }};
    std::ranges::sort(ev, [](const auto& a, const auto& b) { return a.val > b.val; });

    // Normalize eigenvectors
    for (auto& e : ev) {
        const float len = std::sqrt(e.x*e.x + e.y*e.y + e.z*e.z);
        if (len > 1e-10f) { e.x /= len; e.y /= len; e.z /= len; }
    }

    // Project positions onto axes to get exact extents
    auto dot = [](const MeshVertexVec3& a, float ex, float ey, float ez) {
        return a.x * ex + a.y * ey + a.z * ez;
    };

    auto project = [&](float ax, float ay, float az, float& out_min, float& out_max) {
        out_min = std::numeric_limits<float>::max();
        out_max = -std::numeric_limits<float>::max();
        for (const auto& v : positions) {
            MeshVertexVec3 d;
            d.x = v.x - obb_center.x;
            d.y = v.y - obb_center.y;
            d.z = v.z - obb_center.z;
            const float proj = dot(d, ax, ay, az);
            out_min = std::min(out_min, proj);
            out_max = std::max(out_max, proj);
        }
    };

    float min_u, max_u, min_v, max_v, min_w, max_w;
    project(ev[0].x, ev[0].y, ev[0].z, min_u, max_u);
    project(ev[1].x, ev[1].y, ev[1].z, min_v, max_v);
    project(ev[2].x, ev[2].y, ev[2].z, min_w, max_w);

    sm.obb.center = obb_center;
    sm.obb.axis_u = {ev[0].x, ev[0].y, ev[0].z};
    sm.obb.half_extent_u = std::max(std::abs(min_u), std::abs(max_u));
    sm.obb.axis_v = {ev[1].x, ev[1].y, ev[1].z};
    sm.obb.half_extent_v = std::max(std::abs(min_v), std::abs(max_v));
    sm.obb.axis_w = {ev[2].x, ev[2].y, ev[2].z};
    sm.obb.half_extent_w = std::max(std::abs(min_w), std::abs(max_w));
}

} // anonymous namespace

void IMeshLoader::PostProcess(VulkanEngine::Mesh& mesh) {
    for (auto& sm : mesh.subMeshes) {
        ComputeSubmeshBoundingVolumes(mesh.vertices, mesh.indices, sm);
    }
}

} // namespace VulkanEngine::FileLoaders::Mesh
