# 2BoneRectangle.glb

Small two-bone rig used by the `skinned_scene` example. Authored by the
repository owner in Blender and exported with the Khronos glTF Blender I/O
exporter (v5.3.43); no third-party assets.

- 2x2x6 cuboid spanning `z = -3..3`, 80 vertices, 68 triangles.
- Two joints: `Bone` (root, at the +Z end) and `Bone.001` (child, at the seam).
- One skin over both joints with `JOINTS_0` (u8) / `WEIGHTS_0` (f32); the eight
  vertices on the `z = 0` ring are weighted `0.5 / 0.5`, the rest are rigid.
- No animation clips; the example poses the bones in code.
