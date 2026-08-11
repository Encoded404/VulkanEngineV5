module;

export module VulkanEngine.CompilerEngine;

import std;
import ShaderReflection;

export namespace VulkanEngine::ShaderSystem {

struct CompileResult {
    std::vector<std::uint32_t> spirv;
    std::vector<Binding> bindings;
    std::uint64_t binding_hash;
    std::string diagnostics;
};

struct CompileRequest {
    std::string source_path;
    std::string source_text;
    std::string entry_point = "main";
    ShaderStage stage = ShaderStage::eFragment;
    unsigned int optimization_level = 0;
};

class CompilerEngine {
public:
    CompilerEngine() = delete;

    [[nodiscard]] static std::expected<CompileResult, std::string>
        Compile(const CompileRequest& request);
};

} // namespace VulkanEngine::ShaderSystem
