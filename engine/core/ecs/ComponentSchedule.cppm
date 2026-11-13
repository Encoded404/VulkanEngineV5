// The resolved component schedule: conflicts, explicit ordering and levels.
//
// One graph, one resolver, one dump. Explicit `Before`/`After` edges and conflict
// edges live in the same DAG; levels are cut from a Kahn total order; the result
// is frozen at registration close. There is no incremental graph here — the plan
// resolves once, so this module exposes no AddNode/AddEdge and no transactional
// insertion.
//
// Determinism is part of the contract: explicit edges are inserted first, then
// conflicting pairs are visited in a fixed double loop over discovery order, and
// each conflict edge is oriented so it never closes a cycle. The same
// declarations plus the same registration order therefore always produce the
// same levels.
//
// This module depends on ComponentAccess, not on the registry, so it can be
// built and tested before any registry change.

module;

#include <cassert>

export module VulkanEngine.ECS.ComponentSchedule;

import std;

import VulkanEngine.ECS.ComponentAccess;

export namespace VulkanEngine {

// A barrier: every type in level n finishes before any type in level n+1 starts.
struct Level {
    std::vector<std::size_t> type_ids;  // registration order within the level

    friend bool operator==(const Level&, const Level&) = default;
};

// One dispatch step of the execution plan.
struct PlanSegment {
    std::size_t first_level = 0;  // index into Levels()
    std::size_t level_count = 0;  // consecutive levels in this segment
    // True only for a single level whose own work list holds more than one unit.
    // A multi-level segment is sequential and runs its levels in order: the merge
    // is a dispatch optimization, never a relaxation of the barrier.
    bool parallel = false;

    friend bool operator==(const PlanSegment&, const PlanSegment&) = default;
};

// Rule 2: a contested component type.
struct ConflictEdge {
    std::size_t target = 0;
    bool write_write = false;

    friend bool operator==(const ConflictEdge&, const ConflictEdge&) = default;
};

// Rule 3: a contested engine service.
struct ServiceConflictEdge {
    Service service = Service::Input;
    bool write_write = false;

    friend bool operator==(const ServiceConflictEdge&, const ServiceConflictEdge&) = default;
};

struct ExplicitEdge {
    friend bool operator==(const ExplicitEdge&, const ExplicitEdge&) = default;
};

// Rule 1: exclusivity, which conflicts with every other registered type.
struct ExclusiveEdge {
    friend bool operator==(const ExclusiveEdge&, const ExclusiveEdge&) = default;
};

// std::variant's == is deleted unless every alternative has one, so these
// defaulted operators are what make EdgeRecord's defaulted == compile.
using EdgeCause =
    std::variant<ExplicitEdge, ConflictEdge, ServiceConflictEdge, ExclusiveEdge>;

enum class DirectionSource : std::uint8_t {
    Explicit,       // a direct Before/After declaration on the pair
    ExplicitPath,   // along an existing explicit path between the two
    DiscoveryOrder, // the tie-break that does not close a cycle
};

[[nodiscard]] constexpr std::string_view DirectionSourceName(DirectionSource source) noexcept {
    switch (source) {
        case DirectionSource::Explicit:       return "explicit";
        case DirectionSource::ExplicitPath:   return "explicit path";
        case DirectionSource::DiscoveryOrder: return "discovery order";
    }
    return "<unknown>";
}

struct EdgeRecord {
    std::size_t before = 0;  // type id
    std::size_t after = 0;   // type id
    EdgeCause cause = ExplicitEdge{};
    DirectionSource direction = DirectionSource::DiscoveryOrder;

    friend bool operator==(const EdgeRecord&, const EdgeRecord&) = default;
};

// A cycle, formatted once so every rethrow produces the same text.
struct ScheduleError {
    std::vector<std::size_t> cycle;  // the type ids on the explicit-edge cycle, in order
    std::string message;
};

class ComponentSchedule {
public:
    [[nodiscard]] static std::expected<ComponentSchedule, ScheduleError>
    Resolve(std::span<const ComponentAccessInfo> registered, NameTable names);

    [[nodiscard]] std::span<const Level> Levels() const noexcept { return levels_; }
    [[nodiscard]] std::span<const EdgeRecord> Edges() const noexcept { return edges_; }

    // The resolving type's own info, or nullptr when `type_id` is not a node.
    [[nodiscard]] const ComponentAccessInfo* Info(std::size_t type_id) const noexcept;

    [[nodiscard]] std::string_view NameOf(std::size_t type_id) const noexcept;

    [[nodiscard]] std::string Describe() const;

    // Per-step. `units_per_registered_type` is indexed by discovery index. One
    // segment per parallel level, plus one merged sequential segment per run of
    // serial levels; a sequential segment's levels run in order. A segment is a
    // range into Levels(), so the out-parameter overload allocates nothing.
    void ExecutionPlan(std::span<const std::size_t> units_per_registered_type,
                       std::vector<PlanSegment>& out) const;

    // Convenience for tests and tooling.
    [[nodiscard]] std::vector<PlanSegment>
    ExecutionPlan(std::span<const std::size_t> units_per_registered_type) const;

private:
    static constexpr std::size_t kAbsent = std::numeric_limits<std::size_t>::max();

    [[nodiscard]] std::size_t DiscoveryOf(std::size_t type_id) const noexcept;

    std::span<const ComponentAccessInfo> infos_{};
    std::vector<NameEntry> names_{};
    std::vector<Level> levels_{};
    std::vector<EdgeRecord> edges_{};
    std::vector<std::size_t> type_of_{};       // discovery index -> type id
    std::vector<std::size_t> discovery_of_{};  // type id -> discovery index (kAbsent when absent)
};

} // namespace VulkanEngine

// ── Implementation ──
namespace VulkanEngine {

namespace {

using BoolGraph = std::vector<std::vector<bool>>;

// Reusable depth-first reachability over one graph. The stamp vector avoids
// clearing a visited array per query, which matters because the conflict pass
// asks O(n^2) times.
class Reachability {
public:
    Reachability(const BoolGraph& graph, std::size_t count)
        : graph_(graph), stamp_(count, 0) {
        stack_.reserve(count);
    }

    [[nodiscard]] bool operator()(std::size_t from, std::size_t to) {
        if (from == to) return true;
        ++generation_;
        stack_.clear();
        stack_.push_back(from);
        stamp_[from] = generation_;
        while (!stack_.empty()) {
            const std::size_t vertex = stack_.back();
            stack_.pop_back();
            const auto& out = graph_[vertex];
            for (std::size_t candidate = 0; candidate < out.size(); ++candidate) {
                if (!out[candidate] || stamp_[candidate] == generation_) continue;
                if (candidate == to) return true;
                stamp_[candidate] = generation_;
                stack_.push_back(candidate);
            }
        }
        return false;
    }

private:
    const BoolGraph& graph_;
    std::vector<std::uint32_t> stamp_;
    std::vector<std::size_t> stack_;
    std::uint32_t generation_ = 0;
};

// The first id present in both sets. Iterating `lhs` in its own order keeps the
// reported target deterministic regardless of which set is smaller.
[[nodiscard]] bool Intersects(const std::flat_set<std::size_t>& lhs,
                              const std::flat_set<std::size_t>& rhs,
                              std::size_t& shared) {
    for (const std::size_t value : lhs) {
        if (rhs.contains(value)) {
            shared = value;
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::flat_set<std::size_t> Merged(const std::flat_set<std::size_t>& lhs,
                                                const std::flat_set<std::size_t>& rhs) {
    std::flat_set<std::size_t> out;
    out.insert(lhs.begin(), lhs.end());
    out.insert(rhs.begin(), rhs.end());
    return out;
}

} // namespace

std::size_t ComponentSchedule::DiscoveryOf(std::size_t type_id) const noexcept {
    if (type_id >= discovery_of_.size()) return kAbsent;
    return discovery_of_[type_id];
}

const ComponentAccessInfo* ComponentSchedule::Info(std::size_t type_id) const noexcept {
    const std::size_t discovery = DiscoveryOf(type_id);
    if (discovery == kAbsent || discovery >= infos_.size()) return nullptr;
    return &infos_[discovery];
}

std::string_view ComponentSchedule::NameOf(std::size_t type_id) const noexcept {
    for (const NameEntry& entry : names_) {
        if (entry.type_id == type_id) return entry.name;
    }
    return {};
}

std::expected<ComponentSchedule, ScheduleError>
ComponentSchedule::Resolve(std::span<const ComponentAccessInfo> registered, NameTable names) {
    ComponentSchedule schedule;
    schedule.infos_ = registered;

    const std::size_t count = registered.size();

    // ── indices ──
    std::size_t max_type_id = 0;
    for (const ComponentAccessInfo& info : registered) {
        max_type_id = std::max(max_type_id, info.type_id);
    }
    if (count > 0) schedule.discovery_of_.assign(max_type_id + 1, kAbsent);
    schedule.type_of_.reserve(count);
    schedule.names_.reserve(count + names.size());
    for (std::size_t discovery = 0; discovery < count; ++discovery) {
        const ComponentAccessInfo& info = registered[discovery];
        assert(info.discovery_index == discovery &&
               "ComponentSchedule::Resolve expects the infos in registration order");
        schedule.type_of_.push_back(info.type_id);
        schedule.discovery_of_[info.type_id] = discovery;
        if (!info.name.empty()) schedule.names_.push_back({info.type_id, info.name});
    }
    for (const NameEntry& entry : names) schedule.names_.push_back(entry);

    // ── explicit edges ──
    BoolGraph adjacency(count, std::vector<bool>(count, false));
    // `pair_index` records "this unordered pair already has an edge", which is
    // what keeps a conflicting pair down to one edge. The *directed* edge set is
    // separate: two contradictory declarations on one pair (`A Before<B>` plus
    // `B Before<A>`, or `A Before<B>` plus `A After<B>`) must both land in the
    // graph so the cycle check reports them instead of silently honoring the
    // first one.
    std::map<std::pair<std::size_t, std::size_t>, std::size_t> pair_index;
    std::set<std::pair<std::size_t, std::size_t>> directed_edges;
    auto add_edge = [&](std::size_t before, std::size_t after, EdgeCause cause,
                        DirectionSource direction) {
        if (!directed_edges.emplace(before, after).second) return;
        pair_index.emplace(std::pair{std::min(before, after), std::max(before, after)},
                           schedule.edges_.size());
        schedule.edges_.push_back(EdgeRecord{schedule.type_of_[before],
                                             schedule.type_of_[after], std::move(cause),
                                             direction});
        adjacency[before][after] = true;
    };

    for (std::size_t discovery = 0; discovery < count; ++discovery) {
        for (const std::size_t target : registered[discovery].before) {
            const std::size_t other = schedule.DiscoveryOf(target);
            if (other == kAbsent || other == discovery) continue;
            add_edge(discovery, other, ExplicitEdge{}, DirectionSource::Explicit);
        }
        for (const std::size_t target : registered[discovery].after) {
            const std::size_t other = schedule.DiscoveryOf(target);
            if (other == kAbsent || other == discovery) continue;
            add_edge(other, discovery, ExplicitEdge{}, DirectionSource::Explicit);
        }
    }

    // The explicit subgraph is the only place a user-facing cycle can exist:
    // conflict edges are oriented acyclically by construction below.
    const BoolGraph explicit_graph = adjacency;

    {
        std::vector<int> color(count, 0);
        std::vector<std::size_t> path;
        std::vector<std::size_t> cycle;
        auto visit = [&](auto&& self, std::size_t vertex) -> bool {
            color[vertex] = 1;
            path.push_back(vertex);
            for (std::size_t candidate = 0; candidate < count; ++candidate) {
                if (!explicit_graph[vertex][candidate]) continue;
                if (color[candidate] == 1) {
                    const auto start = std::find(path.begin(), path.end(), candidate);
                    cycle.assign(start, path.end());
                    return true;
                }
                if (color[candidate] == 0 && self(self, candidate)) return true;
            }
            path.pop_back();
            color[vertex] = 2;
            return false;
        };

        bool found = false;
        for (std::size_t vertex = 0; vertex < count && !found; ++vertex) {
            if (color[vertex] == 0) found = visit(visit, vertex);
        }

        if (found) {
            ScheduleError error;
            std::string text = "component access declarations form an ordering cycle: ";
            for (std::size_t i = 0; i < cycle.size(); ++i) {
                if (i != 0) text += " -> ";
                const std::size_t type_id = schedule.type_of_[cycle[i]];
                error.cycle.push_back(type_id);
                const std::string_view name = schedule.NameOf(type_id);
                text += name.empty() ? std::format("component_type[{}]", type_id)
                                     : std::string{name};
            }
            if (!cycle.empty()) {
                text += " -> ";
                const std::string_view name = schedule.NameOf(schedule.type_of_[cycle.front()]);
                text += name.empty()
                            ? std::format("component_type[{}]", schedule.type_of_[cycle.front()])
                            : std::string{name};
            }
            error.message = std::move(text);
            return std::unexpected(std::move(error));
        }
    }

    // ── conflict edges ──
    //
    // Visited in a fixed double loop over discovery order (i < j), each pair
    // oriented against the graph built so far. This ordering is part of the
    // contract: it decides which conflict edges exist when each is oriented.
    if (count > 1) {
        std::vector<std::flat_set<std::size_t>> writes;
        std::vector<std::flat_set<std::size_t>> reads;
        writes.reserve(count);
        reads.reserve(count);
        for (const ComponentAccessInfo& info : registered) {
            writes.push_back(Merged(info.self_writes, info.any));
            reads.push_back(Merged(info.self_reads, info.any));
        }

        Reachability reaches(adjacency, count);
        Reachability explicit_reaches(explicit_graph, count);

        auto conflict_of = [&](std::size_t lhs, std::size_t rhs,
                               EdgeCause& cause) -> bool {
            const ComponentAccessInfo& left = registered[lhs];
            const ComponentAccessInfo& right = registered[rhs];
            if (left.concurrency == Concurrency::Exclusive ||
                right.concurrency == Concurrency::Exclusive) {
                cause = ExclusiveEdge{};
                return true;
            }
            std::size_t shared = 0;
            if (Intersects(writes[lhs], writes[rhs], shared)) {
                cause = ConflictEdge{shared, true};
                return true;
            }
            if (Intersects(writes[lhs], reads[rhs], shared)) {
                cause = ConflictEdge{shared, false};
                return true;
            }
            if (Intersects(writes[rhs], reads[lhs], shared)) {
                cause = ConflictEdge{shared, false};
                return true;
            }
            for (std::uint8_t index = 0; index < std::to_underlying(Service::Count); ++index) {
                const Service service = static_cast<Service>(index);
                const bool left_write = (left.services & ServiceWrite(service)) != 0;
                const bool left_read = (left.services & ServiceRead(service)) != 0;
                const bool right_write = (right.services & ServiceWrite(service)) != 0;
                const bool right_read = (right.services & ServiceRead(service)) != 0;
                if ((left_write && (right_read || right_write)) ||
                    (right_write && (left_read || left_write))) {
                    cause = ServiceConflictEdge{service, left_write && right_write};
                    return true;
                }
            }
            return false;
        };

        for (std::size_t lhs = 0; lhs < count; ++lhs) {
            for (std::size_t rhs = lhs + 1; rhs < count; ++rhs) {
                const auto key = std::pair{lhs, rhs};
                if (pair_index.contains(key)) continue;  // already related
                EdgeCause cause;
                if (!conflict_of(lhs, rhs, cause)) continue;

                [[maybe_unused]] const bool forward = reaches(lhs, rhs);
                const bool backward = reaches(rhs, lhs);
                // A DAG cannot reach in both directions, so at most one
                // orientation can close a cycle: this is an internal invariant,
                // not a declaration error.
                assert(!(forward && backward) &&
                       "conflict orientation found both directions reachable");
                const std::size_t before = backward ? rhs : lhs;
                const std::size_t after = backward ? lhs : rhs;
                const DirectionSource direction =
                    explicit_reaches(before, after) ? DirectionSource::ExplicitPath
                                                    : DirectionSource::DiscoveryOrder;
                add_edge(before, after, std::move(cause), direction);
            }
        }
    }

    // ── Kahn's total order and the level cut ──
    std::vector<std::size_t> indegree(count, 0);
    for (std::size_t from = 0; from < count; ++from) {
        for (std::size_t to = 0; to < count; ++to) {
            if (adjacency[from][to]) ++indegree[to];
        }
    }
    std::vector<bool> emitted(count, false);
    std::vector<std::size_t> level_of(count, 0);
    std::size_t level = 0;
    for (std::size_t emitted_count = 0; emitted_count < count; ++emitted_count) {
        std::size_t pick = kAbsent;
        for (std::size_t candidate = 0; candidate < count; ++candidate) {
            if (!emitted[candidate] && indegree[candidate] == 0) {
                pick = candidate;
                break;
            }
        }
        // The graph is acyclic by construction (explicit checked, conflicts
        // oriented acyclically), so a free node always exists.
        assert(pick != kAbsent && "component schedule graph is not acyclic");
        if (pick == kAbsent) break;

        if (emitted_count != 0) {
            // A new level starts whenever the next node has a predecessor
            // already placed in the current level.
            for (std::size_t predecessor = 0; predecessor < count; ++predecessor) {
                if (adjacency[predecessor][pick] && emitted[predecessor] &&
                    level_of[predecessor] == level) {
                    ++level;
                    break;
                }
            }
        }
        emitted[pick] = true;
        level_of[pick] = level;
        for (std::size_t to = 0; to < count; ++to) {
            if (adjacency[pick][to]) --indegree[to];
        }
    }

    if (count != 0) schedule.levels_.resize(level + 1);
    for (std::size_t discovery = 0; discovery < count; ++discovery) {
        schedule.levels_[level_of[discovery]].type_ids.push_back(
            schedule.type_of_[discovery]);
    }

    return schedule;
}

void ComponentSchedule::ExecutionPlan(std::span<const std::size_t> units_per_registered_type,
                                      std::vector<PlanSegment>& out) const {
    out.clear();
    auto units_of_level = [&](std::size_t level_index) {
        std::size_t units = 0;
        for (const std::size_t type_id : levels_[level_index].type_ids) {
            const std::size_t discovery = DiscoveryOf(type_id);
            if (discovery != kAbsent && discovery < units_per_registered_type.size()) {
                units += units_per_registered_type[discovery];
            }
        }
        return units;
    };

    std::size_t level_index = 0;
    while (level_index < levels_.size()) {
        if (units_of_level(level_index) > 1) {
            out.push_back(PlanSegment{level_index, 1, true});
            ++level_index;
            continue;
        }
        // Maximal run of levels whose own work list holds at most one unit.
        const std::size_t first = level_index;
        while (level_index < levels_.size() && units_of_level(level_index) <= 1) {
            ++level_index;
        }
        out.push_back(PlanSegment{first, level_index - first, false});
    }
}

std::vector<PlanSegment>
ComponentSchedule::ExecutionPlan(std::span<const std::size_t> units_per_registered_type) const {
    std::vector<PlanSegment> plan;
    ExecutionPlan(units_per_registered_type, plan);
    return plan;
}

std::string ComponentSchedule::Describe() const {
    std::string out;
    auto append = [&out](std::string_view text) { out.append(text); };
    auto name_or_id = [this](std::size_t type_id) {
        const std::string_view name = NameOf(type_id);
        if (!name.empty()) return std::string{name};
        return std::format("component_type[{}]", type_id);
    };
    auto join_ids = [&](const std::flat_set<std::size_t>& ids) {
        std::string text;
        for (const std::size_t id : ids) {
            if (!text.empty()) text += ',';
            text += name_or_id(id);
        }
        return text.empty() ? std::string{"-"} : text;
    };

    std::format_to(std::back_inserter(out), "component schedule: {} types, {} levels\n",
                   type_of_.size(), levels_.size());

    for (std::size_t index = 0; index < levels_.size(); ++index) {
        const Level& level = levels_[index];
        std::format_to(std::back_inserter(out), "level {} ({} type{}):\n", index,
                       level.type_ids.size(), level.type_ids.size() == 1 ? "" : "s");
        for (const std::size_t type_id : level.type_ids) {
            const ComponentAccessInfo* info = Info(type_id);
            if (info == nullptr) continue;
            std::format_to(std::back_inserter(out),
                           "  {:<24} [{}]  mode:{:<13} self r:{} w:{}", name_or_id(type_id),
                           AccessSourceName(info->source), ConcurrencyName(info->concurrency),
                           join_ids(info->self_reads), join_ids(info->self_writes));
            if (!info->any.empty()) {
                std::format_to(std::back_inserter(out), "  any:{}", join_ids(info->any));
            }
            std::string service_reads;
            std::string service_writes;
            for (std::uint8_t raw = 0; raw < std::to_underlying(Service::Count); ++raw) {
                const Service service = static_cast<Service>(raw);
                const bool readable = (info->services & ServiceRead(service)) != 0;
                const bool writable = (info->services & ServiceWrite(service)) != 0;
                if (writable) {
                    if (!service_writes.empty()) service_writes += ',';
                    service_writes += ServiceName(service);
                } else if (readable) {
                    if (!service_reads.empty()) service_reads += ',';
                    service_reads += ServiceName(service);
                }
            }
            if (!service_reads.empty() || !service_writes.empty()) {
                append("  services");
                if (!service_reads.empty()) {
                    append(" r:");
                    append(service_reads);
                }
                if (!service_writes.empty()) {
                    append(" w:");
                    append(service_writes);
                }
            }
            append("\n");
        }
    }

    append("edges:\n");
    for (const EdgeRecord& edge : edges_) {
        std::string cause_text;
        if (const auto* exclusive = std::get_if<ExclusiveEdge>(&edge.cause)) {
            (void)exclusive;
            cause_text = "exclusive";
        } else if (const auto* conflict = std::get_if<ConflictEdge>(&edge.cause)) {
            cause_text = std::format("conflict: {} {}", conflict->write_write ? "write/write"
                                                                             : "read/write",
                                     name_or_id(conflict->target));
        } else if (const auto* service = std::get_if<ServiceConflictEdge>(&edge.cause)) {
            cause_text = std::format("conflict: service {} {}",
                                     service->write_write ? "write/write" : "read/write",
                                     ServiceName(service->service));
        } else {
            cause_text = "explicit";
        }
        append("  ");
        append(name_or_id(edge.before));
        append(" -> ");
        append(name_or_id(edge.after));
        append("  (");
        append(cause_text);
        if (edge.direction != DirectionSource::Explicit) {
            append(", ");
            append(DirectionSourceName(edge.direction));
        }
        append(")\n");
    }

    return out;
}

} // namespace VulkanEngine
