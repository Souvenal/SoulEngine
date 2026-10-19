module;

// magic_enum::enum_flags_test is not exported by the v0.9.8 module.
#include <magic_enum/magic_enum_flags.hpp>

module RHI:Pipeline.impl;

import Core;
import Shader;
import magic_enum;

import :Pipeline;
import :Ref;

import std;

namespace SoulEngine {

/// @brief The shader-level type a bound resource plays in this request.
///
/// Derived from the view's role — one view, one role.
[[nodiscard]] auto ResolveBindingType(const RHIBoundResource& Bound) -> ShaderResourceType {
    if (const auto* View = std::get_if<RHITextureView>(&Bound))
        return ShaderResourceType::Texture;
    if (const auto* View = std::get_if<RHIBufferView>(&Bound)) {
        if (magic_enum::enum_flags_test(View->Usage, RHIBufferUsage::Constant))
            return ShaderResourceType::ConstantBuffer;
        return ShaderResourceType::StorageBuffer;
    }
    if (std::get_if<RHISamplerView>(&Bound))
        return ShaderResourceType::Sampler;
    if (std::get_if<RHITlasView>(&Bound))
        return ShaderResourceType::AccelerationStructure;
    return ShaderResourceType::Unknown;
}

/// @brief The access direction a bound view exercises.
///
/// A storage texture view writes, every other texture view reads. Buffer, sampler, and TLAS views answer
/// Unknown, and access validation skips Unknown on either side.
[[nodiscard]] auto ResolveBindingAccess(const RHIBoundResource& Bound) -> ShaderBindingAccess {
    if (const auto* View = std::get_if<RHITextureView>(&Bound))
        return View->Usage == RHITextureUsage::Storage ? ShaderBindingAccess::ReadWrite : ShaderBindingAccess::Read;
    return ShaderBindingAccess::Unknown;
}

/// @brief Readiness probe shared by binding validation: the resource a bound alternative refers to.
///
/// Returns nullptr when the alternative is absent or its payload is not Ready.
[[nodiscard]] auto ResolveBindingObject(const RHIBoundResource& Bound) -> RHIObject* {
    if (const auto* View = std::get_if<RHITextureView>(&Bound))
        return View->Texture.TryGet();
    if (const auto* View = std::get_if<RHIBufferView>(&Bound))
        return View->Buffer.TryGet();
    if (const auto* View = std::get_if<RHISamplerView>(&Bound))
        return View->Sampler.TryGet();
    if (const auto* View = std::get_if<RHITlasView>(&Bound))
        return View->Tlas.TryGet();
    return nullptr;
}

auto RHIShaderBindingSet::HasUnboundBindings() const -> bool {
    return std::ranges::any_of(m_BindingsBySet, [](const auto& Bindings) {
        return std::ranges::any_of(Bindings, [](const Slot& Binding) {
            return std::holds_alternative<std::monostate>(Binding.Bound) && Binding.ArrayElements.empty();
        });
    });
}

auto RHIShaderBindingSet::BindResources(std::span<const RHIShaderBindingRequest> Resources)
    -> std::expected<void, ErrorMessage> {
    if (Resources.empty())
        return std::unexpected(ErrorMessage("Shader binding resource batch must not be empty"));

    std::vector<Slot*> FlatBindings;
    for (auto& SetBindings : m_BindingsBySet)
        for (auto& Binding : SetBindings)
            FlatBindings.push_back(&Binding);

    // We first query each request's set and index
    std::vector<std::pair<Uint32, Uint32>> BindingLocations;
    BindingLocations.reserve(Resources.size());
    for (const auto& Request : Resources) {
        const auto It = std::ranges::find_if(
            FlatBindings, [&](const Slot* Slot) { return Slot->Info.ParameterPath == Request.ParameterPath; });
        if (It == FlatBindings.end())
            return std::unexpected(ErrorMessage(
                Format("Shader parameter '{}' is not present in the shader binding layout", Request.ParameterPath)));
        const auto Location = std::pair{(*It)->Info.Set, (*It)->Info.BindingIndex};
        BindingLocations.push_back(Location);
        if (!ResolveBindingObject(Request.Bound))
            return std::unexpected(
                ErrorMessage(Format("Shader parameter '{}' has no ready resource", Request.ParameterPath)));
        const auto BoundType = ResolveBindingType(Request.Bound);
        if (BoundType != (*It)->Info.Type)
            return std::unexpected(ErrorMessage(Format("Shader parameter '{}' expects {}, received {}",
                                                       Request.ParameterPath,
                                                       magic_enum::enum_name((*It)->Info.Type),
                                                       magic_enum::enum_name(BoundType))));
        const auto BoundAccess = ResolveBindingAccess(Request.Bound);
        if (BoundAccess != ShaderBindingAccess::Unknown && (*It)->Info.Access != ShaderBindingAccess::Unknown &&
            BoundAccess != (*It)->Info.Access)
            return std::unexpected(ErrorMessage(Format("Shader parameter '{}' expects {} access, received {}",
                                                       Request.ParameterPath,
                                                       magic_enum::enum_name((*It)->Info.Access),
                                                       magic_enum::enum_name(BoundAccess))));
        if ((*It)->Info.ArrayCount != 1)
            return std::unexpected(ErrorMessage(
                Format("Shader parameter '{}' is an array; use BindArrayResources instead", Request.ParameterPath)));
    }

    // Then we check if all set index are the same
    const auto SetIndex = BindingLocations.front().first;
    for (Uint32 Index = 0; Index < BindingLocations.size(); ++Index)
        for (Uint32 Previous = 0; Previous < Index; ++Previous)
            if (BindingLocations[Index] == BindingLocations[Previous])
                return std::unexpected(
                    ErrorMessage(Format("Shader parameter '{}' appears more than once in a binding batch",
                                        Resources[Index].ParameterPath)));
    if (std::ranges::any_of(BindingLocations,
                            [SetIndex](const auto& Location) { return Location.first != SetIndex; })) {
        String Details;
        for (Uint32 Index = 0; Index < Resources.size(); ++Index) {
            if (!Details.empty())
                Details += ", ";
            Details += Format("'{}' (set {}, binding {})",
                              Resources[Index].ParameterPath,
                              BindingLocations[Index].first,
                              BindingLocations[Index].second);
        }
        return std::unexpected(
            ErrorMessage(Format("Shader binding batch contains multiple descriptor sets: {}", Details)));
    }
    // Sets that contain array slots are completed by separate BindArrayResources batches, so the
    // whole-set count check only applies to purely scalar sets.
    const bool SetHasArraySlots = std::ranges::any_of(m_BindingsBySet[SetIndex],
                                                      [](const Slot& Binding) { return Binding.Info.ArrayCount != 1; });
    if (!SetHasArraySlots && Resources.size() != m_BindingsBySet[SetIndex].size())
        return std::unexpected(ErrorMessage(Format("Shader binding set {} requires {} resources, received {}",
                                                   SetIndex,
                                                   m_BindingsBySet[SetIndex].size(),
                                                   Resources.size())));

    // After checking, we write the resource info into the slot
    for (Uint32 Index = 0; Index < Resources.size(); ++Index) {
        const auto [BindingSet, BindingIndex] = BindingLocations[Index];
        auto&      SetBindings                = m_BindingsBySet[BindingSet];
        const auto Slot = std::ranges::find_if(SetBindings, [BindingIndex](const RHIShaderBindingSet::Slot& Candidate) {
            return Candidate.Info.BindingIndex == BindingIndex;
        });
        Slot->Bound     = Resources[Index].Bound;
    }
    return OnResourcesBound(SetIndex);
}

auto RHIShaderBindingSet::BindArrayResources(std::span<const RHIShaderArrayBindingRequest> Requests)
    -> std::expected<void, ErrorMessage> {
    if (Requests.empty())
        return std::unexpected(ErrorMessage("Shader array binding batch must not be empty"));

    struct ResolvedRequest {
        Slot*                               Target  = nullptr;
        const RHIShaderArrayBindingRequest* Request = nullptr;
    };
    std::vector<ResolvedRequest> Resolved;
    Resolved.reserve(Requests.size());
    std::optional<Uint32> SetIndex = std::nullopt;

    for (const auto& Request : Requests) {
        if (Request.Elements.empty())
            return std::unexpected(
                ErrorMessage(Format("Shader array parameter '{}' has no elements", Request.ParameterPath)));

        Slot* Target = nullptr;
        for (auto& SetBindings : m_BindingsBySet)
            for (auto& Binding : SetBindings)
                if (Binding.Info.ParameterPath == Request.ParameterPath) {
                    Target = &Binding;
                    break;
                }
        if (!Target)
            return std::unexpected(ErrorMessage(
                Format("Shader parameter '{}' is not present in the shader binding layout", Request.ParameterPath)));
        if (Target->Info.ArrayCount == 1)
            return std::unexpected(ErrorMessage(
                Format("Shader parameter '{}' is not an array; use BindResources instead", Request.ParameterPath)));
        const auto BoundType = ResolveBindingType(Request.Elements.front().View);
        if (BoundType != Target->Info.Type)
            return std::unexpected(ErrorMessage(Format("Shader parameter '{}' expects {}, received {}",
                                                       Request.ParameterPath,
                                                       magic_enum::enum_name(Target->Info.Type),
                                                       magic_enum::enum_name(BoundType))));
        const auto BoundAccess = ResolveBindingAccess(Request.Elements.front().View);
        if (BoundAccess != ShaderBindingAccess::Unknown && Target->Info.Access != ShaderBindingAccess::Unknown &&
            BoundAccess != Target->Info.Access)
            return std::unexpected(ErrorMessage(Format("Shader parameter '{}' expects {} access, received {}",
                                                       Request.ParameterPath,
                                                       magic_enum::enum_name(Target->Info.Access),
                                                       magic_enum::enum_name(BoundAccess))));
        if (!SetIndex)
            SetIndex = Target->Info.Set;
        else if (*SetIndex != Target->Info.Set)
            return std::unexpected(
                ErrorMessage(Format("Shader array binding batch contains multiple descriptor sets: '{}' (set {})",
                                    Request.ParameterPath,
                                    Target->Info.Set)));

        // Reject duplicate (path, index) pairs and out-of-capacity indices.
        std::vector<Uint32> SeenIndices;
        SeenIndices.reserve(Request.Elements.size());
        for (const auto& Element : Request.Elements) {
            if (!Element.View.Texture.TryGet())
                return std::unexpected(ErrorMessage(Format(
                    "Shader array parameter '{}' has an element with no ready resource", Request.ParameterPath)));
            if (std::ranges::find(SeenIndices, Element.ArrayIndex) != SeenIndices.end())
                return std::unexpected(ErrorMessage(Format("Shader array parameter '{}' binds index {} more than once",
                                                           Request.ParameterPath,
                                                           Element.ArrayIndex)));
            SeenIndices.push_back(Element.ArrayIndex);
            if (Element.ArrayIndex >= Target->ArrayCapacity)
                return std::unexpected(
                    ErrorMessage(Format("Shader array parameter '{}' index {} exceeds the array capacity of {}",
                                        Request.ParameterPath,
                                        Element.ArrayIndex,
                                        Target->ArrayCapacity)));
        }
        Resolved.push_back(ResolvedRequest{.Target = Target, .Request = &Request});
    }

    for (auto& [Target, Request] : Resolved) {
        Target->ArrayElements.assign(Request->Elements.begin(), Request->Elements.end());
    }
    return OnResourcesBound(*SetIndex);
}

RHIShaderBindingSet::RHIShaderBindingSet(String Name, const RHIShaderBindingSetDesc& Desc)
    : RHIObject(std::move(Name)),
      m_BindlessSpace(Desc.Reflection.BindlessSpace),
      m_PushConstantRanges(Desc.Reflection.PushConstants),
      m_Stages(Desc.Stages) {
    // Step 1: Resolve set count
    Uint32 SetCount = 0;
    // +1 because set index start with 0
    for (const auto& Reflected : Desc.Reflection.Bindings)
        SetCount = std::max(SetCount, Reflected.Set + 1);
    if (m_BindlessSpace)
        SetCount = std::max(SetCount, *m_BindlessSpace + 1);

    // Step 2: sort all bindings by set and binding index
    m_BindingsBySet.resize(SetCount);
    for (const auto& Reflected : Desc.Reflection.Bindings)
        m_BindingsBySet[Reflected.Set].push_back(Slot{.Info = Reflected});
    for (auto& SetBindings : m_BindingsBySet)
        std::ranges::sort(SetBindings, [](const auto& Left, const auto& Right) {
            return Left.Info.BindingIndex < Right.Info.BindingIndex;
        });
}

auto ValidateSpecializationConstants(std::span<const RHISpecializationConstant> Values,
                                     const ShaderReflection& Reflection) -> std::expected<void, ErrorMessage> {
    std::vector<StringView> Seen;
    Seen.reserve(Values.size());
    for (const auto& Value : Values) {
        if (std::ranges::find(Seen, StringView{Value.Name}) != Seen.end())
            return std::unexpected(
                ErrorMessage(Format("Specialization constant '{}' is provided more than once", Value.Name)));
        Seen.push_back(Value.Name);

        const auto It = std::ranges::find_if(Reflection.SpecializationConstants,
                                             [&](const auto& Reflected) { return Reflected.Name == Value.Name; });
        if (It == Reflection.SpecializationConstants.end())
            return std::unexpected(ErrorMessage(
                Format("Specialization constant '{}' is not present in the shader reflection", Value.Name)));

        const auto Matches = [](const auto& Held, ShaderScalarType Type) {
            using HeldType = std::decay_t<decltype(Held)>;
            if constexpr (std::same_as<HeldType, bool>)
                return Type == ShaderScalarType::Bool;
            else if constexpr (std::same_as<HeldType, Int32>)
                return Type == ShaderScalarType::Int32;
            else if constexpr (std::same_as<HeldType, Uint32>)
                return Type == ShaderScalarType::Uint32;
            else
                return Type == ShaderScalarType::Float32;
        };
        if (!std::visit([&](const auto& Held) { return Matches(Held, It->ScalarType); }, Value.Value))
            return std::unexpected(ErrorMessage(
                Format("Specialization constant '{}' holds a value of the wrong scalar type (reflected as {})",
                       Value.Name,
                       magic_enum::enum_name(It->ScalarType))));
    }
    return {};
}

} // namespace SoulEngine
