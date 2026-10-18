module;

#include <entt/entt.hpp>

module Core:ECS.impl;

import std;
import :ECS;

namespace SoulEngine {

[[nodiscard]] auto SystemScheduler::CompileDependency() -> std::expected<void, ErrorMessage> {
    if (m_Compiled)
        return std::unexpected(ErrorMessage("CompileDependency() has already been called"));

    const auto Count = static_cast<Uint32>(m_Entries.size());

    // Build and deduplicate edges, collecting every dangling reference before failing so one run reports
    // all of them.
    std::flat_set<std::pair<Uint32, Uint32>> Edges;
    String                                   Dangling;
    for (Uint32 I = 0; I < Count; ++I) {
        const auto& Entry = m_Entries[I];
        for (const auto& Target : Entry.Before) {
            if (const auto It = m_NameToIndex.find(Target); It != m_NameToIndex.end())
                Edges.emplace(I, It->second);
            else
                Dangling += Format("\n  '{}' declares Before '{}', which is not registered", Entry.Name, Target);
        }
        for (const auto& Target : Entry.After) {
            if (const auto It = m_NameToIndex.find(Target); It != m_NameToIndex.end())
                Edges.emplace(It->second, I);
            else
                Dangling += Format("\n  '{}' declares After '{}', which is not registered", Entry.Name, Target);
        }
    }
    if (!Dangling.empty())
        return std::unexpected(ErrorMessage("Unknown system dependency references:" + Dangling));

    std::vector<std::vector<Uint32>> Outgoing(Count);
    std::vector<std::vector<Uint32>> Incoming(Count);
    std::vector<Uint32>              Indegree(Count, 0);
    for (const auto& [From, To] : Edges) {
        Outgoing[From].push_back(To);
        Incoming[To].push_back(From);
        ++Indegree[To];
    }

    // The min-heap on registration index keeps the order deterministic across platforms and runs.
    std::priority_queue<Uint32, std::vector<Uint32>, std::greater<Uint32>> Ready;
    for (Uint32 I = 0; I < Count; ++I)
        if (Indegree[I] == 0)
            Ready.push(I);

    std::vector<Uint32> Order;
    Order.reserve(Count);
    while (!Ready.empty()) {
        const Uint32 I = Ready.top();
        Ready.pop();
        Order.push_back(I);
        for (const Uint32 To : Outgoing[I])
            if (--Indegree[To] == 0)
                Ready.push(To);
    }

    if (Order.size() != Count) {
        // Every still-blocked node has at least one blocked predecessor, so walking backwards from any of
        // them must revisit a node; that segment is one concrete cycle.
        const auto DescribeCycle = [&]() -> String {
            Uint32 Start = 0;
            while (Indegree[Start] == 0)
                ++Start;
            std::vector<Uint32> Path;
            Uint32              Current = Start;
            while (std::ranges::find(Path, Current) == Path.end()) {
                Path.push_back(Current);
                for (const Uint32 Predecessor : Incoming[Current])
                    if (Indegree[Predecessor] > 0) {
                        Current = Predecessor;
                        break;
                    }
            }
            String     Cycle;
            const auto Begin = std::ranges::find(Path, Current);
            for (auto It = Begin; It != Path.end(); ++It)
                Cycle += m_Entries[*It].Name + " -> ";
            return Cycle + m_Entries[Current].Name;
        };
        return std::unexpected(ErrorMessage(Format("System dependency cycle detected: {}", DescribeCycle())));
    }

    m_Order.clear();
    m_Order.reserve(Count);
    for (const Uint32 I : Order)
        m_Order.push_back(m_Entries[I].System.get());
    m_Compiled = true;
    return {};
}

[[nodiscard]] auto SystemScheduler::OnUpdate(Float32 DeltaTime) -> std::expected<void, ErrorMessage> {
    if (!m_Compiled)
        return std::unexpected(ErrorMessage("CompileDependency() must succeed before OnUpdate()"));
    for (auto* System : m_Order)
        System->OnUpdate(DeltaTime);
    return {};
}

auto SystemScheduler::SetupObservers() -> void {
    for (auto& Entry : m_Entries)
        if (Entry.System)
            Entry.System->SetupObservers();
}

auto SystemScheduler::TeardownObservers() -> void {
    for (auto& Entry : m_Entries)
        if (Entry.System)
            Entry.System->TeardownObservers();
}

auto SystemScheduler::ClearObservers() -> void {
    for (auto& Entry : m_Entries)
        if (Entry.System)
            Entry.System->ClearObservers();
}

} // namespace SoulEngine
