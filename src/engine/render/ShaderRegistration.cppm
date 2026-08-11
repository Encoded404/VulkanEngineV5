module;

export module VulkanEngine.ShaderRegistration;

import std;
import ShaderReflection;

import VulkanEngine.ShaderManager;

import Shaders.Engine.ExpandComp;
import Shaders.Engine.OcclusionCullComp;
import Shaders.Engine.HizGenComp;
import Shaders.Engine.CollectCountCompactComp;
import Shaders.Engine.CollectWriteComp;
import Shaders.Engine.MainIndirVert;
import Shaders.Engine.DepthIndirVert;
import Shaders.Engine.StandardMeshFrag;
import Shaders.Engine.DepthPrepassFrag;

export namespace VulkanEngine {

struct EngineShaderIds {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    ShaderSystem::ShaderId expand_comp;
    ShaderSystem::ShaderId occlusion_cull_comp;
    ShaderSystem::ShaderId hiz_gen_comp;
    ShaderSystem::ShaderId collect_count_compact_comp;
    ShaderSystem::ShaderId collect_write_comp;
    ShaderSystem::ShaderId main_indir_vert;
    ShaderSystem::ShaderId depth_indir_vert;
    ShaderSystem::ShaderId standard_mesh_frag;
    ShaderSystem::ShaderId depth_prepass_frag;
    // NOLINTEND(misc-non-private-member-variables-in-classes)

    void RegisterAll(ShaderSystem::ShaderManager& mgr, std::string_view data_dir) {
        expand_comp              = Shaders::Engine::ExpandComp::Register(mgr, data_dir);
        occlusion_cull_comp      = Shaders::Engine::OcclusionCullComp::Register(mgr, data_dir);
        hiz_gen_comp             = Shaders::Engine::HizGenComp::Register(mgr, data_dir);
        collect_count_compact_comp = Shaders::Engine::CollectCountCompactComp::Register(mgr, data_dir);
        collect_write_comp       = Shaders::Engine::CollectWriteComp::Register(mgr, data_dir);
        main_indir_vert          = Shaders::Engine::MainIndirVert::Register(mgr, data_dir);
        depth_indir_vert         = Shaders::Engine::DepthIndirVert::Register(mgr, data_dir);
        standard_mesh_frag       = Shaders::Engine::StandardMeshFrag::Register(mgr, data_dir);
        depth_prepass_frag       = Shaders::Engine::DepthPrepassFrag::Register(mgr, data_dir);
    }
};

} // namespace VulkanEngine
