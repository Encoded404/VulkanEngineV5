#pragma once

// Synthetic glTF/GLB fixtures for the skeletal-import tests.
//
// The fixtures are built byte-for-byte here rather than checked in as binary
// assets: every accessor is visible in the test source, so a failure names the
// exact field the importer misread. The GLB container and the two fixtures below
// are shared by the skin, animation and scene-import tests.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace GltfSkeletonFixture {

inline void AppendU32(std::vector<std::byte>& out, const std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFu));
}

inline void AppendU16(std::vector<std::byte>& out, const std::uint16_t v) {
    out.push_back(static_cast<std::byte>(v & 0xFFu));
    out.push_back(static_cast<std::byte>((v >> 8) & 0xFFu));
}

inline void AppendU8(std::vector<std::byte>& out, const std::uint8_t v) {
    out.push_back(static_cast<std::byte>(v));
}

inline void AppendFloat(std::vector<std::byte>& out, const float v) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    AppendU32(out, bits);
}

inline void AppendMat4Identity(std::vector<std::byte>& out) {
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            AppendFloat(out, row == column ? 1.0f : 0.0f);
        }
    }
}

[[nodiscard]] inline std::size_t Pad4(const std::size_t n) { return (4u - (n % 4u)) % 4u; }

// A minimal GLB: 12-byte header, one JSON chunk, one BIN chunk. The JSON is
// padded with spaces and the BIN with zeros, as the spec requires.
[[nodiscard]] inline std::vector<std::byte> MakeGlb(const std::string& json,
                                                    const std::vector<std::byte>& bin) {
    std::vector<std::byte> json_chunk;
    json_chunk.reserve(json.size() + 4);
    for (const char c : json) json_chunk.push_back(static_cast<std::byte>(c));
    json_chunk.insert(json_chunk.end(), Pad4(json_chunk.size()), std::byte{0x20});

    std::vector<std::byte> bin_chunk = bin;
    bin_chunk.insert(bin_chunk.end(), Pad4(bin_chunk.size()), std::byte{0x00});

    std::vector<std::byte> out;
    out.push_back(std::byte{'g'});
    out.push_back(std::byte{'l'});
    out.push_back(std::byte{'T'});
    out.push_back(std::byte{'F'});
    AppendU32(out, 2u);
    AppendU32(out, static_cast<std::uint32_t>(12u + 8u + json_chunk.size() + 8u + bin_chunk.size()));
    AppendU32(out, static_cast<std::uint32_t>(json_chunk.size()));
    AppendU32(out, 0x4E4F534Au); // 'JSON'
    out.insert(out.end(), json_chunk.begin(), json_chunk.end());
    AppendU32(out, static_cast<std::uint32_t>(bin_chunk.size()));
    AppendU32(out, 0x004E4942u); // 'BIN\0'
    out.insert(out.end(), bin_chunk.begin(), bin_chunk.end());
    return out;
}

// Writes `bytes` into `dir` under `name`; returns the path.
[[nodiscard]] inline std::filesystem::path WriteFixture(const std::filesystem::path& dir,
                                                        const std::string& name,
                                                        const std::span<const std::byte> bytes) {
    std::filesystem::create_directories(dir);
    const auto path = dir / name;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    file.close();
    return path;
}

// ── shared geometry prefix ─────────────────────────────────────────────────
//
// One triangle with two vertices at the same position but different joints, so
// skinned dedup is exercised. Layout (offsets in bytes):
//   0   POSITION   3 * VEC3 f32        36
//   36  NORMAL     3 * VEC3 f32        36
//   72  JOINTS_0   3 * VEC4 u8         12
//   84  WEIGHTS_0  3 * VEC4 u8 norm    12
//   96  indices    3 * u16              6   (+2 pad)
//   104 IBM        2 * MAT4 f32       128
// total 232.
inline constexpr std::uint32_t kGeometryEnd = 232;

[[nodiscard]] inline std::vector<std::byte> GeometryBin() {
    std::vector<std::byte> bin;
    // v0 and v1 share a position on purpose: they are bound to different joints.
    const float positions[9] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    for (const float f : positions) AppendFloat(bin, f);
    for (int i = 0; i < 3; ++i) {
        AppendFloat(bin, 0.0f);
        AppendFloat(bin, 0.0f);
        AppendFloat(bin, 1.0f);
    }
    const std::uint8_t joints[12] = {
        0, 0, 0, 0,
        1, 0, 0, 0,
        0, 1, 0, 0,
    };
    for (const std::uint8_t v : joints) AppendU8(bin, v);
    const std::uint8_t weights[12] = {
        255, 0, 0, 0,
        255, 0, 0, 0,
        128, 127, 0, 0,
    };
    for (const std::uint8_t v : weights) AppendU8(bin, v);
    AppendU16(bin, 0);
    AppendU16(bin, 1);
    AppendU16(bin, 2);
    while (bin.size() % 4 != 0) bin.push_back(std::byte{0});
    AppendMat4Identity(bin);
    AppendMat4Identity(bin);
    return bin;
}

inline constexpr const char* kGeometryBufferViews = R"(
    {"buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962},
    {"buffer": 0, "byteOffset": 36, "byteLength": 36, "target": 34962},
    {"buffer": 0, "byteOffset": 72, "byteLength": 12, "target": 34962},
    {"buffer": 0, "byteOffset": 84, "byteLength": 12, "target": 34962},
    {"buffer": 0, "byteOffset": 96, "byteLength": 6, "target": 34963},
    {"buffer": 0, "byteOffset": 104, "byteLength": 128})";

inline constexpr const char* kGeometryAccessors = R"(
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
    {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3"},
    {"bufferView": 2, "componentType": 5121, "count": 3, "type": "VEC4"},
    {"bufferView": 3, "componentType": 5121, "normalized": true, "count": 3, "type": "VEC4"},
    {"bufferView": 4, "componentType": 5123, "count": 3, "type": "SCALAR"},
    {"bufferView": 5, "componentType": 5126, "count": 2, "type": "MAT4"})";

// ── skin-import fixture ────────────────────────────────────────────────────
//
// Nodes 0..2 are an armature (translated +10 on X) over two joints. Nodes 3, 4
// and 5 all reference the same glTF mesh but with different skins: skins 0 and 1
// share the same ordered joint-node set (so they must share one Skeleton), while
// skin 2 covers only joint0 (so it gets its own). Node 3 is translated far away
// to prove a skinned placement ignores its own node transform.
[[nodiscard]] inline std::vector<std::byte> SkinnedGlb() {
    const std::string json = std::string(R"json({
  "asset": {"version": "2.0"},
  "scene": 0,
  "scenes": [{"nodes": [0, 3, 4, 5]}],
  "nodes": [
    {"name": "armature", "translation": [10.0, 0.0, 0.0], "children": [1]},
    {"name": "joint0", "translation": [1.0, 0.0, 0.0], "children": [2]},
    {"name": "joint1", "translation": [0.0, 1.0, 0.0]},
    {"mesh": 0, "skin": 0, "translation": [99.0, 99.0, 99.0]},
    {"mesh": 0, "skin": 1},
    {"mesh": 0, "skin": 2}
  ],
  "skins": [
    {"joints": [1, 2], "inverseBindMatrices": 5, "skeleton": 0},
    {"joints": [1, 2], "inverseBindMatrices": 5, "skeleton": 0},
    {"joints": [1], "inverseBindMatrices": 5, "skeleton": 0}
  ],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "JOINTS_0": 2, "WEIGHTS_0": 3}, "indices": 4, "mode": 4}]}],
  "buffers": [{"byteLength": )json") + std::to_string(kGeometryEnd) + R"json(}],
  "bufferViews": [)json" + kGeometryBufferViews + R"json(],
  "accessors": [)json" + kGeometryAccessors + R"json(]
})json";
    return MakeGlb(json, GeometryBin());
}

// ── animation fixture ──────────────────────────────────────────────────────
//
// Shares the geometry prefix, then adds:
//   232 times         2 * f32           8
//   240 translation   2 * VEC3 f32     24
//   264 rotation      2 * VEC4 f32     32
//   296 cubic spline  6 * VEC3 f32     72
// Node 4 is a second skeleton's only joint. Node 1 is joint0 of skin 0, node 2
// joint1; node 3 is the skinned mesh node and is not a joint (a dropped-target
// probe).
[[nodiscard]] inline std::vector<std::byte> AnimatedGlb() {
    std::vector<std::byte> bin = GeometryBin();
    AppendFloat(bin, 0.0f);
    AppendFloat(bin, 1.0f);
    // translation keys: (0,0,0) and (1,0,0)
    AppendFloat(bin, 0.0f); AppendFloat(bin, 0.0f); AppendFloat(bin, 0.0f);
    AppendFloat(bin, 1.0f); AppendFloat(bin, 0.0f); AppendFloat(bin, 0.0f);
    // rotation keys: identity and 90 degrees about Z
    AppendFloat(bin, 0.0f); AppendFloat(bin, 0.0f); AppendFloat(bin, 0.0f); AppendFloat(bin, 1.0f);
    AppendFloat(bin, 0.0f); AppendFloat(bin, 0.0f); AppendFloat(bin, 0.70710678f); AppendFloat(bin, 0.70710678f);
    // cubic-spline triples for key 0 and key 1; only the middle values ((1,2,3)
    // and (4,5,6)) are kept.
    const float cubic[18] = {
        9.0f, 9.0f, 9.0f, 1.0f, 2.0f, 3.0f, 7.0f, 7.0f, 7.0f,
        8.0f, 8.0f, 8.0f, 4.0f, 5.0f, 6.0f, 6.0f, 6.0f, 6.0f,
    };
    for (const float f : cubic) AppendFloat(bin, f);

    const std::string json = std::string(R"json({
  "asset": {"version": "2.0"},
  "scene": 0,
  "scenes": [{"nodes": [0, 3, 4]}],
  "nodes": [
    {"name": "armature", "translation": [10.0, 0.0, 0.0], "children": [1]},
    {"name": "joint0", "translation": [1.0, 0.0, 0.0], "children": [2]},
    {"name": "joint1", "translation": [0.0, 1.0, 0.0]},
    {"mesh": 0, "skin": 0, "translation": [99.0, 99.0, 99.0]},
    {"name": "second_root", "translation": [0.0, 5.0, 0.0]}
  ],
  "skins": [
    {"joints": [1, 2], "inverseBindMatrices": 5, "skeleton": 0},
    {"joints": [4], "inverseBindMatrices": 5, "skeleton": 4}
  ],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "JOINTS_0": 2, "WEIGHTS_0": 3}, "indices": 4, "mode": 4}]}],
  "animations": [
    {"name": "walk",
     "channels": [
       {"sampler": 0, "target": {"node": 1, "path": "translation"}},
       {"sampler": 1, "target": {"node": 2, "path": "rotation"}},
       {"sampler": 0, "target": {"node": 3, "path": "scale"}}
     ],
     "samplers": [
       {"input": 6, "output": 7, "interpolation": "LINEAR"},
       {"input": 6, "output": 8, "interpolation": "LINEAR"}
     ]},
    {"name": "cubic",
     "channels": [{"sampler": 0, "target": {"node": 1, "path": "translation"}}],
     "samplers": [{"input": 6, "output": 9, "interpolation": "CUBICSPLINE"}]},
    {"name": "multi",
     "channels": [
       {"sampler": 0, "target": {"node": 1, "path": "translation"}},
       {"sampler": 0, "target": {"node": 4, "path": "translation"}}
     ],
     "samplers": [{"input": 6, "output": 7, "interpolation": "LINEAR"}]}
  ],
  "buffers": [{"byteLength": )json") + std::to_string(bin.size()) + R"json(}],
  "bufferViews": [)json" + kGeometryBufferViews + R"json(,
    {"buffer": 0, "byteOffset": 232, "byteLength": 8},
    {"buffer": 0, "byteOffset": 240, "byteLength": 24},
    {"buffer": 0, "byteOffset": 264, "byteLength": 32},
    {"buffer": 0, "byteOffset": 296, "byteLength": 72}],
  "accessors": [)json" + kGeometryAccessors + R"json(,
    {"bufferView": 6, "componentType": 5126, "count": 2, "type": "SCALAR"},
    {"bufferView": 7, "componentType": 5126, "count": 2, "type": "VEC3"},
    {"bufferView": 8, "componentType": 5126, "count": 2, "type": "VEC4"},
    {"bufferView": 9, "componentType": 5126, "count": 6, "type": "VEC3"}]
})json";
    return MakeGlb(json, bin);
}

// ── overlapping joint sets fixture ─────────────────────────────────────────
//
// Skin 0 covers [joint0, joint1] and skin 1 covers only [joint0], so the two
// skeletons have different ordered joint lists but share node 1. An animation
// targeting node 1 must reach *both* skeletons' clips.
[[nodiscard]] inline std::vector<std::byte> OverlappingSkeletonGlb() {
    std::vector<std::byte> bin = GeometryBin();
    AppendFloat(bin, 0.0f); // times
    AppendFloat(bin, 1.0f);
    AppendFloat(bin, 0.0f); AppendFloat(bin, 0.0f); AppendFloat(bin, 0.0f); // translation keys
    AppendFloat(bin, 1.0f); AppendFloat(bin, 0.0f); AppendFloat(bin, 0.0f);

    const std::string json = std::string(R"json({
  "asset": {"version": "2.0"},
  "scene": 0,
  "scenes": [{"nodes": [0, 3, 4]}],
  "nodes": [
    {"name": "rig", "children": [1]},
    {"name": "joint0", "translation": [1.0, 0.0, 0.0], "children": [2]},
    {"name": "joint1", "translation": [0.0, 1.0, 0.0]},
    {"mesh": 0, "skin": 0},
    {"mesh": 0, "skin": 1}
  ],
  "skins": [
    {"joints": [1, 2], "inverseBindMatrices": 5},
    {"joints": [1], "inverseBindMatrices": 5}
  ],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "JOINTS_0": 2, "WEIGHTS_0": 3}, "indices": 4, "mode": 4}]}],
  "animations": [
    {"name": "overlap",
     "channels": [{"sampler": 0, "target": {"node": 1, "path": "translation"}}],
     "samplers": [{"input": 6, "output": 7, "interpolation": "LINEAR"}]}
  ],
  "buffers": [{"byteLength": )json") + std::to_string(bin.size()) + R"json(}],
  "bufferViews": [)json" + kGeometryBufferViews + R"json(,
    {"buffer": 0, "byteOffset": 232, "byteLength": 8},
    {"buffer": 0, "byteOffset": 240, "byteLength": 24}],
  "accessors": [)json" + kGeometryAccessors + R"json(,
    {"bufferView": 6, "componentType": 5126, "count": 2, "type": "SCALAR"},
    {"bufferView": 7, "componentType": 5126, "count": 2, "type": "VEC3"}]
})json";
    return MakeGlb(json, bin);
}

// ── malformed skin fixture ─────────────────────────────────────────────────
//
// The only skin's joint list is [valid_joint, missing_node]. The list is
// positional (JOINTS_n indexes it and the inverse binds align with it), so a
// partial import would shift every later joint; the skin is skipped whole. The
// node still has a transform and must be placed with it, not identity-placed as
// if the broken skin resolved.
[[nodiscard]] inline std::vector<std::byte> SkippedSkinGlb() {
    const std::string json = std::string(R"json({
  "asset": {"version": "2.0"},
  "scene": 0,
  "scenes": [{"nodes": [0]}],
  "nodes": [
    {"mesh": 0, "skin": 0, "translation": [1.0, 2.0, 3.0]},
    {"name": "valid_joint"}
  ],
  "skins": [{"joints": [1, 99], "inverseBindMatrices": 5}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "JOINTS_0": 2, "WEIGHTS_0": 3}, "indices": 4, "mode": 4}]}],
  "buffers": [{"byteLength": )json") + std::to_string(kGeometryEnd) + R"json(}],
  "bufferViews": [)json" + kGeometryBufferViews + R"json(],
  "accessors": [)json" + kGeometryAccessors + R"json(]
})json";
    return MakeGlb(json, GeometryBin());
}

// ── two influence sets fixture ─────────────────────────────────────────────
//
// One joint per set: JOINTS_0/WEIGHTS_0 and JOINTS_1/WEIGHTS_1 both present, so
// the mesh reports `influence_sets == 2` and carries a parallel second stream.
[[nodiscard]] inline std::vector<std::byte> TwoSetGlb() {
    std::vector<std::byte> bin;
    const float positions[9] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    for (const float f : positions) AppendFloat(bin, f);
    for (int i = 0; i < 3; ++i) {
        AppendFloat(bin, 0.0f); AppendFloat(bin, 0.0f); AppendFloat(bin, 1.0f);
    }
    const std::uint8_t joints0[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    const std::uint8_t weights0[12] = {204, 0, 0, 0, 204, 0, 0, 0, 204, 0, 0, 0};
    const std::uint8_t joints1[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    const std::uint8_t weights1[12] = {51, 0, 0, 0, 51, 0, 0, 0, 51, 0, 0, 0};
    for (const std::uint8_t v : joints0) AppendU8(bin, v);
    for (const std::uint8_t v : weights0) AppendU8(bin, v);
    for (const std::uint8_t v : joints1) AppendU8(bin, v);
    for (const std::uint8_t v : weights1) AppendU8(bin, v);
    AppendU16(bin, 0); AppendU16(bin, 1); AppendU16(bin, 2);
    while (bin.size() % 4 != 0) bin.push_back(std::byte{0});
    // offsets: positions 0, normals 36, joints0 72, weights0 84, joints1 96,
    // weights1 108, indices 120, IBM 128.
    AppendMat4Identity(bin);
    AppendMat4Identity(bin);

    const std::string json = std::string(R"json({
  "asset": {"version": "2.0"},
  "scene": 0,
  "scenes": [{"nodes": [0, 3]}],
  "nodes": [
    {"name": "rig", "children": [1, 2]},
    {"name": "joint0"},
    {"name": "joint1"},
    {"mesh": 0, "skin": 0}
  ],
  "skins": [{"joints": [1, 2], "inverseBindMatrices": 7}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "JOINTS_0": 2, "WEIGHTS_0": 3, "JOINTS_1": 4, "WEIGHTS_1": 5}, "indices": 6, "mode": 4}]}],
  "buffers": [{"byteLength": )json") + std::to_string(bin.size()) + R"json(}],
  "bufferViews": [
    {"buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962},
    {"buffer": 0, "byteOffset": 36, "byteLength": 36, "target": 34962},
    {"buffer": 0, "byteOffset": 72, "byteLength": 12, "target": 34962},
    {"buffer": 0, "byteOffset": 84, "byteLength": 12, "target": 34962},
    {"buffer": 0, "byteOffset": 96, "byteLength": 12, "target": 34962},
    {"buffer": 0, "byteOffset": 108, "byteLength": 12, "target": 34962},
    {"buffer": 0, "byteOffset": 120, "byteLength": 6, "target": 34963},
    {"buffer": 0, "byteOffset": 128, "byteLength": 128}
  ],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
    {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3"},
    {"bufferView": 2, "componentType": 5121, "count": 3, "type": "VEC4"},
    {"bufferView": 3, "componentType": 5121, "normalized": true, "count": 3, "type": "VEC4"},
    {"bufferView": 4, "componentType": 5121, "count": 3, "type": "VEC4"},
    {"bufferView": 5, "componentType": 5121, "normalized": true, "count": 3, "type": "VEC4"},
    {"bufferView": 6, "componentType": 5123, "count": 3, "type": "SCALAR"},
    {"bufferView": 7, "componentType": 5126, "count": 2, "type": "MAT4"}
  ]
})json";
    return MakeGlb(json, bin);
}

} // namespace GltfSkeletonFixture
