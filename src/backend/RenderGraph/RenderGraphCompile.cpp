module;

#include <algorithm>
#include <cstdint>
#include <queue>
#include <ranges>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <vulkan/vulkan.hpp>

module VulkanBackend.RenderGraph;

namespace VulkanEngine::RenderGraph {

CompiledRenderGraph RenderGraphBuilder::Compile() const {
    CompiledRenderGraph result{};

    auto emit_diagnostic = [&](DiagnosticCode code, DiagnosticSeverity severity, std::string message) {
        result.diagnostics.push_back(CompileDiagnostic{
            .code = code,
            .severity = severity,
            .message = std::move(message),
        });
    };

    if (passes_.empty()) {
        result.success = true;
        emit_diagnostic(
            DiagnosticCode::EmptyGraph,
            DiagnosticSeverity::Info,
            "Render graph has no passes; compile produced an empty execution plan.");
        return result;
    }

    for (const auto& resource : resources_) {
        if (resource.has_initial_state && !IsResourceStateCompatible(resource.kind, resource.initial_state)) {
            emit_diagnostic(
                DiagnosticCode::InvalidInitialState,
                DiagnosticSeverity::Error,
                "Render graph compile failed: resource '" + resource.name + "' has an incompatible initial state.");
        }
        if (resource.has_final_state && !IsResourceStateCompatible(resource.kind, resource.final_state)) {
            emit_diagnostic(
                DiagnosticCode::InvalidFinalState,
                DiagnosticSeverity::Error,
                "Render graph compile failed: resource '" + resource.name + "' has an incompatible final state.");
        }
    }

    const size_t pass_count = passes_.size();
    std::vector<std::unordered_set<uint32_t>> edges(pass_count);
    std::vector<uint32_t> indegree(pass_count, 0);

    auto add_edge = [&](uint32_t from, uint32_t to) {
        if (from == to) {
            return;
        }
        if (edges[from].insert(to).second) {
            ++indegree[to];
        }
    };

    for (const auto& [before, after] : explicit_dependencies_) {
        if (!IsValidPassHandle(before) || !IsValidPassHandle(after)) {
            emit_diagnostic(
                DiagnosticCode::InvalidExplicitDependency,
                DiagnosticSeverity::Error,
                "Render graph contains an invalid explicit pass dependency.");
            continue;
        }

        if (!passes_[before.index].enabled || !passes_[after.index].enabled) {
            continue;
        }

        add_edge(before.index, after.index);
    }

    struct ResourceTracker {
        int32_t last_writer = -1;
        std::unordered_set<uint32_t> readers{};
    };

    std::vector<ResourceTracker> resource_trackers(resources_.size());

    for (uint32_t pass_index = 0; pass_index < pass_count; ++pass_index) {
        const auto& pass = passes_[pass_index];
        if (!pass.enabled) {
            continue;
        }

        for (const auto& read : pass.reads) {
            if (!IsValidResourceHandle(read)) {
                emit_diagnostic(
                    DiagnosticCode::InvalidReadHandle,
                    DiagnosticSeverity::Error,
                    "Pass '" + pass.name + "' references an invalid read resource handle.");
                continue;
            }

            auto& tracker = resource_trackers[read.index];
            if (tracker.last_writer >= 0) {
                add_edge(static_cast<uint32_t>(tracker.last_writer), pass_index);
            }
            tracker.readers.insert(pass_index);
        }

        for (const auto& write : pass.writes) {
            if (!IsValidResourceHandle(write)) {
                emit_diagnostic(
                    DiagnosticCode::InvalidWriteHandle,
                    DiagnosticSeverity::Error,
                    "Pass '" + pass.name + "' references an invalid write resource handle.");
                continue;
            }

            auto& tracker = resource_trackers[write.index];
            if (tracker.last_writer >= 0) {
                add_edge(static_cast<uint32_t>(tracker.last_writer), pass_index);
            }
            for (const uint32_t reader_index : tracker.readers) {
                add_edge(reader_index, pass_index);
            }
            tracker.readers.clear();
            tracker.last_writer = static_cast<int32_t>(pass_index);
        }
    }

    std::queue<uint32_t> ready{};
    for (uint32_t pass_index = 0; pass_index < pass_count; ++pass_index) {
        if (!passes_[pass_index].enabled) {
            continue;
        }
        if (indegree[pass_index] == 0) {
            ready.push(pass_index);
        }
    }

    std::vector<uint32_t> sorted_indices{};
    while (!ready.empty()) {
        const uint32_t current = ready.front();
        ready.pop();
        sorted_indices.push_back(current);

        for (const uint32_t dependent : edges[current]) {
            if (--indegree[dependent] == 0) {
                ready.push(dependent);
            }
        }
    }

    const uint32_t enabled_count = static_cast<uint32_t>(std::ranges::count_if(passes_, [](const PassNode& pass) {
        return pass.enabled;
    }));

    if (sorted_indices.size() != enabled_count) {
        emit_diagnostic(
            DiagnosticCode::CycleDetected,
            DiagnosticSeverity::Error,
            "Render graph compile failed: pass dependency cycle detected.");
        result.success = false;
        return result;
    }

    struct ResourcePassState {
        ResourceState state{};
        bool has_state = false;
    };

    std::vector<ResourcePassState> resource_states(resources_.size());

    for (uint32_t resource_index = 0; resource_index < resources_.size(); ++resource_index) {
        const auto& resource = resources_[resource_index];
        if (resource.has_initial_state) {
            resource_states[resource_index] = {resource.initial_state, true};
        }
    }

    std::vector<CompiledPass> compiled_passes;
    compiled_passes.reserve(sorted_indices.size());

    auto make_transition = [&](uint32_t res_idx, const ResourceState& target) -> ResourceTransition {
        ResourceTransition t{};
        t.resource_index = res_idx;
        t.target_state = target;
        return t;
    };

    for (const unsigned int pass_index : sorted_indices) {
        const auto& pass = passes_[pass_index];
        const PassHandle handle{.index = pass_index, .generation = pass.generation};

        CompiledPass compiled_pass{};
        compiled_pass.handle = handle;
        compiled_pass.name = pass.name;
        compiled_pass.queue = pass.queue;
        compiled_pass.execute = pass.execute;
        compiled_pass.attachment_setup = pass.attachment_setup;

        for (const auto& write : pass.writes) {
            if (!IsValidResourceHandle(write)) continue;
            const auto& resource = resources_[write.index];
            if (resource.kind != ResourceKind::Image) continue;

            auto& current_state = resource_states[write.index];

            ResourceState target_state;
            if (pass.attachment_setup && pass.attachment_setup->depth_attachment &&
                       pass.attachment_setup->depth_attachment->resource.index == write.index) {
                target_state = ResourceState::ImageState(
                    PipelineStageIntent::DepthAttachment, AccessIntent::Write,
                    QueueType::Graphics, ImageLayoutIntent::DepthAttachment);
            } else {
                target_state = ResourceState::ImageState(
                    PipelineStageIntent::ColorAttachment, AccessIntent::Write,
                    QueueType::Graphics, ImageLayoutIntent::ColorAttachment);
            }

            if (!current_state.has_state) {
                const ResourceState undefined_state = ResourceState::ImageState(
                    PipelineStageIntent::TopOfPipe, AccessIntent::None,
                    QueueType::Graphics, ImageLayoutIntent::Undefined);
                if (!StatesEqual(undefined_state, target_state)) {
                    compiled_pass.pre_pass_transitions.push_back(
                        make_transition(write.index, target_state));
                }
            } else if (!StatesEqual(current_state.state, target_state)) {
                compiled_pass.pre_pass_transitions.push_back(
                    make_transition(write.index, target_state));
            }

            current_state.state = target_state;
            current_state.has_state = true;
        }

        for (const auto& read : pass.reads) {
            if (!IsValidResourceHandle(read)) continue;
            const auto& resource = resources_[read.index];
            if (resource.kind != ResourceKind::Image) continue;

            auto& current_state = resource_states[read.index];

            ResourceState target_state;
            if (resource.image_info && (resource.image_info->usage & vk::ImageUsageFlagBits::eDepthStencilAttachment)) {
                target_state = ResourceState::ImageState(
                    PipelineStageIntent::DepthAttachment, AccessIntent::Read,
                    QueueType::Graphics, ImageLayoutIntent::DepthAttachment);
            } else {
                target_state = ResourceState::ImageState(
                    PipelineStageIntent::FragmentShader, AccessIntent::Read,
                    QueueType::Graphics, ImageLayoutIntent::ShaderReadOnly);
            }

            if (!current_state.has_state) {
                const ResourceState undefined_state = ResourceState::ImageState(
                    PipelineStageIntent::TopOfPipe, AccessIntent::None,
                    QueueType::Graphics, ImageLayoutIntent::Undefined);
                if (!StatesEqual(undefined_state, target_state)) {
                    compiled_pass.pre_pass_transitions.push_back(
                        make_transition(read.index, target_state));
                }
            } else if (!StatesEqual(current_state.state, target_state)) {
                compiled_pass.pre_pass_transitions.push_back(
                    make_transition(read.index, target_state));
            }

            current_state.state = target_state;
            current_state.has_state = true;
        }

        compiled_passes.push_back(std::move(compiled_pass));
    }

    for (size_t ordered_index = 0; ordered_index < sorted_indices.size(); ++ordered_index) {
        const uint32_t pass_index = sorted_indices[ordered_index];
        const auto& pass = passes_[pass_index];

        for (const auto& write : pass.writes) {
            if (!IsValidResourceHandle(write)) continue;
            const auto& resource = resources_[write.index];
            if (resource.kind != ResourceKind::Image || !resource.has_final_state) continue;

            const int32_t current_oi = static_cast<int32_t>(ordered_index);
            int32_t last_usage = -1;
            for (int32_t oi = static_cast<int32_t>(sorted_indices.size()) - 1; oi > current_oi; --oi) {
                const auto& p = passes_[sorted_indices[static_cast<size_t>(oi)]];
                const ResourceHandle h{.index = write.index, .generation = resource.generation};
                if (ContainsResource(p.writes, h) || ContainsResource(p.reads, h)) {
                    last_usage = oi;
                    break;
                }
            }

            if (last_usage < 0) {
                auto& current_state = resource_states[write.index];
                if (current_state.has_state && !StatesEqual(current_state.state, resource.final_state)) {
                    compiled_passes[ordered_index].post_pass_transitions.push_back(
                        make_transition(write.index, resource.final_state));
                }
            }
        }
    }

    result.passes = std::move(compiled_passes);

    result.resource_lifetimes.reserve(resources_.size());
    result.resource_info.reserve(resources_.size());
    result.initial_states.resize(resources_.size());
    result.has_initial_state.resize(resources_.size(), false);
    result.resource_images.resize(resources_.size());
    result.resource_formats.resize(resources_.size(), vk::Format::eUndefined);

    for (uint32_t resource_index = 0; resource_index < resources_.size(); ++resource_index) {
        const auto& resource = resources_[resource_index];

        int32_t first = -1;
        int32_t last = -1;
        for (size_t ordered_index = 0; ordered_index < sorted_indices.size(); ++ordered_index) {
            const auto pass_index = sorted_indices[ordered_index];
            const auto& pass = passes_[pass_index];
            const ResourceHandle handle{.index = resource_index, .generation = resource.generation};
            if (ContainsResource(pass.reads, handle) || ContainsResource(pass.writes, handle)) {
                if (first < 0) {
                    first = static_cast<int32_t>(ordered_index);
                }
                last = static_cast<int32_t>(ordered_index);
            }
        }

        result.resource_lifetimes.push_back(ResourceLifetime{
            .handle = ResourceHandle{.index = resource_index, .generation = resource.generation},
            .name = resource.name,
            .imported = resource.imported,
            .transient = resource.transient,
            .first_pass = first,
            .last_pass = last,
        });

        result.resource_info.push_back(ResourceInfo{
            .name = resource.name,
            .kind = resource.kind,
            .imported = resource.imported,
            .image_info = resource.image_info,
            .buffer_info = resource.buffer_info,
        });

        if (resource.image_info) {
            result.resource_formats[resource_index] = resource.image_info->format;
        }

        if (resource.has_initial_state) {
            result.initial_states[resource_index] = resource.initial_state;
            result.has_initial_state[resource_index] = true;
        }
    }

    const bool has_errors = std::ranges::any_of(result.diagnostics, [](const CompileDiagnostic& diagnostic) {
        return diagnostic.severity == DiagnosticSeverity::Error;
    });

    result.success = !has_errors;
    if (result.success) {
        emit_diagnostic(
            DiagnosticCode::CompileSuccess,
            DiagnosticSeverity::Info,
            "Render graph compile succeeded.");
    }

    return result;
}

}  // namespace VulkanEngine::RenderGraph
