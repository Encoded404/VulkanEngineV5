module;

#include <logging/logging_macros.hpp>

module VulkanEngine.ModifierManager;

import std;
import std.compat;

import logiface;

import VulkanEngine.ModifierId;

namespace VulkanEngine::Modifiers {

namespace {

[[nodiscard]] bool IsValidShader(const ShaderSystem::ShaderId id) {
    return id != ShaderSystem::kInvalidShaderId;
}

} // namespace

bool ModifierManager::Validate(const ModifierDesc& desc) {
    const std::string label = desc.name.empty() ? "<unnamed>" : desc.name;

    if (!IsValidShader(desc.compute_shader)) {
        LOGIFACE_LOG(error, "ModifierManager: modifier '" + label +
            "' has no compute shader; registration rejected");
        return false;
    }
    if (desc.param_size > kMaxParamBytes) {
        LOGIFACE_LOG(error, "ModifierManager: modifier '" + label + "' requests " +
            std::to_string(desc.param_size) + " parameter bytes; the limit is " +
            std::to_string(kMaxParamBytes));
        return false;
    }
    // The parameter cursor aligns with a single mask, so the alignment has to be
    // a power of two; below 4 it cannot hold a word, above 256 it would waste more
    // than it aligns.
    if (desc.param_alignment < 4 || desc.param_alignment > 256 ||
        !std::has_single_bit(desc.param_alignment)) {
        LOGIFACE_LOG(error, "ModifierManager: modifier '" + label + "' has parameter alignment " +
            std::to_string(desc.param_alignment) +
            "; it must be a power of two in [4, 256]");
        return false;
    }
    if (desc.workgroup_size == 0 || desc.workgroup_size > kMaxWorkgroupSize) {
        LOGIFACE_LOG(error, "ModifierManager: modifier '" + label + "' has workgroup size " +
            std::to_string(desc.workgroup_size) + "; it must be in [1, " +
            std::to_string(kMaxWorkgroupSize) + "]");
        return false;
    }
    return true;
}

ModifierId ModifierManager::Insert(ModifierDesc desc) {
    std::uint32_t slot;
    if (!free_slots_.empty()) {
        slot = free_slots_.back();
        free_slots_.pop_back();
        // Recycling the slot invalidates every handle minted for the previous
        // occupant: same value, different generation.
        ++entries_[slot].id.generation;
    } else {
        slot = static_cast<std::uint32_t>(entries_.size());
        entries_.emplace_back();
    }

    Entry& entry = entries_[slot];
    const std::uint32_t generation = entry.id.generation;
    entry.id = ModifierId{.value = slot, .generation = generation};
    entry.desc = std::move(desc);
    entry.alive = true;
    ++registry_version_;

    if (entry.desc.builtin && entry.desc.name == kSkinningName) {
        skinning_id_ = entry.id;
    }
    return entry.id;
}

ModifierId ModifierManager::Register(ModifierDesc desc) {
    // Only RegisterBuiltin may mint a builtin; an app cannot mark its own entry
    // unremovable.
    desc.builtin = false;
    if (!Validate(desc)) {
        return kInvalidModifierId;
    }
    return Insert(std::move(desc));
}

ModifierId ModifierManager::RegisterBuiltin(ModifierDesc desc) {
    desc.builtin = true;
    if (!Validate(desc)) {
        return kInvalidModifierId;
    }
    // Exactly one builtin may own the reserved name; a second would make
    // SkinningId() ambiguous between two pipelines.
    if (desc.name == kSkinningName && skinning_id_.IsValid()) {
        LOGIFACE_LOG(error, "ModifierManager: builtin modifier '" + desc.name +
            "' is already registered; registration rejected");
        return kInvalidModifierId;
    }
    return Insert(std::move(desc));
}

void ModifierManager::Unregister(const ModifierId id) {
    if (!id.IsValid() || id.value >= entries_.size()) {
        return;
    }
    Entry& entry = entries_[id.value];
    if (!entry.alive || entry.id.generation != id.generation) {
        return;
    }
    if (entry.desc.builtin) {
        LOGIFACE_LOG(warn, "ModifierManager: builtin modifier '" + entry.desc.name +
            "' cannot be unregistered");
        return;
    }
    if (skinning_id_ == id) {
        skinning_id_ = kInvalidModifierId;
    }
    entry.alive = false;
    ++entry.id.generation;
    entry.desc = ModifierDesc{};
    free_slots_.push_back(id.value);
    ++registry_version_;
}

void ModifierManager::Reset() {
    entries_.clear();
    free_slots_.clear();
    skinning_id_ = kInvalidModifierId;
    ++registry_version_;
}

const ModifierDesc* ModifierManager::Get(const ModifierId id) const {
    if (!id.IsValid() || id.value >= entries_.size()) {
        return nullptr;
    }
    const Entry& entry = entries_[id.value];
    if (!entry.alive || entry.id.generation != id.generation) {
        return nullptr;
    }
    return &entry.desc;
}

bool ModifierManager::IsAlive(const ModifierId id) const {
    return Get(id) != nullptr;
}

} // namespace VulkanEngine::Modifiers
