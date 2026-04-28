module;

#include <algorithm>
#include <cstdint>
#include <queue>
#include <ranges>
#include <string>
#include <utility>
#include <unordered_set>
#include <vector>

#include "logging/logging.hpp"

module VulkanEngine.RenderGraph;

namespace VulkanEngine::RenderGraph {

namespace {

bool ContainsResource(const std::vector<ResourceHandle>& handles, ResourceHandle value) {
    return std::ranges::find(handles, value) != handles.end();
}

bool ContainsDependency(const std::vector<std::pair<PassHandle, PassHandle>>& deps,
                        const std::pair<PassHandle, PassHandle>& value) {
    return std::ranges::find(deps, value) != deps.end();
}

bool IsResourceStateCompatible(ResourceKind kind, const ResourceState& state) {
    if (kind == ResourceKind::Image) {
        return state.has_image_layout;
    }
    return !state.has_image_layout;
}

}  // namespace

void CompileResult::Execute(const void* user_data) const {
    if (!success) {
        return;
    }

    for (const auto& pass : executable_passes) {
        if (pass.execute) {
            pass.execute(user_data);
        }
    }
}

ResourceHandle RenderGraphBuilder::CreateTransientResource(std::string name, ResourceKind kind) {
    const uint32_t index = static_cast<uint32_t>(resources_.size());
    resources_.push_back(ResourceNode{
        .name = std::move(name),
        .kind = kind,
        .generation = 1,
        .imported = false,
        .transient = true,
    });

    return ResourceHandle{.index = index, .generation = resources_.back().generation};
}

ResourceHandle RenderGraphBuilder::ImportResource(std::string name, ResourceKind kind) {
    LOGIFACE_LOG(trace, "Importing resource '" + name + "'");

    const uint32_t index = static_cast<uint32_t>(resources_.size());
    resources_.push_back(ResourceNode{
        .name = std::move(name),
        .kind = kind,
        .generation = 1,
        .imported = true,
        .transient = false,
    });

    return ResourceHandle{.index = index, .generation = resources_.back().generation};
}

bool RenderGraphBuilder::SetInitialState(ResourceHandle resource, ResourceState state) {
    if (!IsValidResourceHandle(resource)) {
        return false;
    }

    auto& resource_node = resources_[resource.index];
    if (!IsResourceStateCompatible(resource_node.kind, state)) {
        return false;
    }

    resource_node.initial_state = state;
    resource_node.has_initial_state = true;
    return true;
}

bool RenderGraphBuilder::SetFinalState(ResourceHandle resource, ResourceState state) {
    if (!IsValidResourceHandle(resource)) {
        return false;
    }

    auto& resource_node = resources_[resource.index];
    if (!IsResourceStateCompatible(resource_node.kind, state)) {
        return false;
    }

    resource_node.final_state = state;
    resource_node.has_final_state = true;
    return true;
}

PassHandle RenderGraphBuilder::AddPass(std::string name, QueueType queue, bool enabled, PassExecutionCallback execute) {
    LOGIFACE_LOG(trace, "Adding pass '" + name + "' with queue type " + std::to_string(static_cast<int>(queue)));

    const uint32_t index = static_cast<uint32_t>(passes_.size());
    passes_.push_back(PassNode{
        .name = std::move(name),
        .queue = queue,
        .generation = 1,
        .enabled = enabled,
        .execute = std::move(execute),
    });

    return PassHandle{.index = index, .generation = passes_.back().generation};
}

bool RenderGraphBuilder::AddRead(PassHandle pass, ResourceHandle resource) {
    if (!IsValidPassHandle(pass) || !IsValidResourceHandle(resource)) {
        return false;
    }

    auto& pass_node = passes_[pass.index];
    if (!ContainsResource(pass_node.reads, resource)) {
        pass_node.reads.push_back(resource);
    }

    return true;
}

bool RenderGraphBuilder::AddWrite(PassHandle pass, ResourceHandle resource) {
    LOGIFACE_LOG(trace, "Adding write to pass '" + passes_[pass.index].name + "' for resource '" + resources_[resource.index].name + "'");

    if (!IsValidPassHandle(pass) || !IsValidResourceHandle(resource)) {
        return false;
    }

    auto& pass_node = passes_[pass.index];
    if (!ContainsResource(pass_node.writes, resource)) {
        pass_node.writes.push_back(resource);
    }

    LOGIFACE_LOG(trace, "returning from AddWrite successfully");
    return true;
}

bool RenderGraphBuilder::AddDependency(PassHandle before, PassHandle after) {
    if (!IsValidPassHandle(before) || !IsValidPassHandle(after) || before == after) {
        return false;
    }

    const std::pair<PassHandle, PassHandle> dependency{before, after};
    if (!ContainsDependency(explicit_dependencies_, dependency)) {
        explicit_dependencies_.emplace_back(before, after);
    }

    return true;
}

CompileResult RenderGraphBuilder::Compile() const {
    CompileResult result{};

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

    result.execution_order.reserve(sorted_indices.size());
    result.executable_passes.reserve(sorted_indices.size());
    for (const uint32_t index : sorted_indices) {
        const PassHandle handle{.index = index, .generation = passes_[index].generation};
        result.execution_order.push_back(handle);
        result.executable_passes.push_back(ExecutablePass{
            .handle = handle,
            .name = passes_[index].name,
            .queue = passes_[index].queue,
            .execute = passes_[index].execute,
        });
    }

    result.resource_lifetimes.reserve(resources_.size());
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

bool RenderGraphBuilder::IsValidResourceHandle(ResourceHandle handle) const {
    if (!handle.IsValid()) {
        return false;
    }

    if (handle.index >= resources_.size()) {
        return false;
    }

    return resources_[handle.index].generation == handle.generation;
}

bool RenderGraphBuilder::IsValidPassHandle(PassHandle handle) const {
    if (!handle.IsValid()) {
        return false;
    }

    if (handle.index >= passes_.size()) {
        return false;
    }

    return passes_[handle.index].generation == handle.generation;
}

}  // namespace VulkanEngine::RenderGraph







