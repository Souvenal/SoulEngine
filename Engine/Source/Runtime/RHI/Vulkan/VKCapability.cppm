module;

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

export module Vulkan:Capability;

import Core;
import vulkan;
import std;

using namespace SoulEngine::Core;

namespace SoulEngine::RHI::Vulkan {

// ═════════════════════════════════════════════════════════════════════════════
// Feature merge helper (defined before CapabilityEntry — used by MergeWith)
// ═════════════════════════════════════════════════════════════════════════════

/// Vulkan feature structs are standard-layout: sType (4B) + padding (4B) +
/// pNext (8B) on 64-bit, then contiguous VkBool32 fields.  We OR the bool
/// portion, leaving sType/pNext untouched.
template<typename T>
auto OrFeatures(T& Dst, const T& Src) -> void {
    static_assert(std::is_standard_layout_v<T>);
    constexpr size_t STypeSize = sizeof(vk::StructureType);
    constexpr size_t Align     = alignof(void*);
    constexpr size_t PNextOff  = (STypeSize + Align - 1) & ~(Align - 1);
    constexpr size_t FirstBool = PNextOff + sizeof(void*);

    for (auto O = FirstBool; O < sizeof(T); O += sizeof(VkBool32)) {
        auto& D = *reinterpret_cast<VkBool32*>(reinterpret_cast<char*>(&Dst) + O);
        auto  S = *reinterpret_cast<const VkBool32*>(reinterpret_cast<const char*>(&Src) + O);
        D = D || S;
    }
}


// ═════════════════════════════════════════════════════════════════════════════
// CapabilityNode
// ═════════════════════════════════════════════════════════════════════════════

struct CapabilityNode {
    virtual ~CapabilityNode() = default;
    virtual auto Clone() const -> UPtr<CapabilityNode>     = 0;
    virtual auto GetPNext() const -> const void*           = 0;
    virtual auto GetSType() const -> vk::StructureType       = 0;
    virtual auto Name() const -> const char*               = 0;
    virtual auto Required() const -> bool                  = 0;
    virtual auto Enabled() const -> bool                   = 0;
    virtual auto SetEnabled(bool V) -> void                = 0;
    virtual auto MergeWith(const CapabilityNode& Src) -> void = 0;
};

template<typename T>
struct CapabilityEntry;

template<typename T>
auto MakeCap(const char* Ext, bool Req, T Init) -> UPtr<CapabilityNode>;

auto MakeCap(const char* Ext, bool Req) -> UPtr<CapabilityNode>;

/// Extension-only entry — no feature struct.
template<>
struct CapabilityEntry<void> final : CapabilityNode {
    const char* m_ExtensionName = nullptr;
    bool        m_IsRequired    = false;
    bool        m_IsEnabled     = false;

    CapabilityEntry(const char* Ext, bool Req) : m_ExtensionName(Ext), m_IsRequired(Req) {}
    auto Clone() const -> UPtr<CapabilityNode> override {
        return MakeCap(m_ExtensionName, m_IsRequired);
    }
    auto MergeWith(const CapabilityNode&) -> void override {}
    auto GetPNext() const -> const void*     override { return nullptr; }
    auto GetSType() const -> vk::StructureType override { return static_cast<vk::StructureType>(VK_STRUCTURE_TYPE_MAX_ENUM); }
    auto Name() const -> const char*         override { return m_ExtensionName; }
    auto Required() const -> bool            override { return m_IsRequired; }
    auto Enabled() const -> bool             override { return m_IsEnabled; }
    auto SetEnabled(bool V) -> void          override { m_IsEnabled = V; }
};

/// Entry pairing an extension gate with a typed Vulkan feature struct.
template<typename T>
struct CapabilityEntry final : CapabilityNode {
    const char* m_ExtensionName = nullptr;
    bool        m_IsRequired    = false;
    bool        m_IsEnabled     = false;
    T           m_Features{};

    CapabilityEntry(const char* Ext, bool Req, T Init)
        : m_ExtensionName(Ext), m_IsRequired(Req), m_Features(std::move(Init)) {}
    auto Clone() const -> UPtr<CapabilityNode> override {
        return MakeCap(m_ExtensionName, m_IsRequired, m_Features);
    }
    auto MergeWith(const CapabilityNode& Src) -> void override {
        OrFeatures(m_Features, static_cast<const CapabilityEntry&>(Src).m_Features);
    }

    auto GetPNext() const -> const void*     override { return &m_Features; }
    auto GetSType() const -> vk::StructureType override { return T::structureType; }
    auto Name() const -> const char*         override { return m_ExtensionName; }
    auto Required() const -> bool            override { return m_IsRequired; }
    auto Enabled() const -> bool             override { return m_IsEnabled; }
    auto SetEnabled(bool V) -> void          override { m_IsEnabled = V; }
};

template<typename T>
auto MakeCap(const char* Ext, bool Req, T Init) -> UPtr<CapabilityNode> {
    Init.sType = T::structureType;
    return std::make_unique<CapabilityEntry<T>>(Ext, Req, std::move(Init));
}

/// Overload for extension-only entries (no feature struct).
inline auto MakeCap(const char* Ext, bool Req) -> UPtr<CapabilityNode> {
    return std::make_unique<CapabilityEntry<void>>(Ext, Req);
}

/// Type-erased access to a Vulkan feature struct's pNext field via the
/// shared initial sequence of VkPhysicalDevice*Features C structs.
struct PNextAccess { vk::StructureType sType; void* pNext; };

// ═════════════════════════════════════════════════════════════════════════════
// Capability
// ═════════════════════════════════════════════════════════════════════════════

class Capability {
  public:
    Capability() {
        RegisterInstanceExtensions();
        RegisterDeviceCapabilities();
    }

    // ── Phase 1: Resolve instance extensions ─────────────────────────────

    [[nodiscard]] auto ResolveInstanceExtensions(vk::raii::Context& Ctx)
        -> std::expected<std::vector<const char*>, ErrorMessage> {
        // Merge GLFW-required extensions — all are required.
        // If a GLFW extension was already declared compile-time, just
        // upgrade it to IsRequired; otherwise append a new request.
        uint32_t GlfwCount = 0;
        auto*    GlfwExts  = glfwGetRequiredInstanceExtensions(&GlfwCount);
        if (!GlfwExts) {
            const char* Desc = nullptr;
            glfwGetError(&Desc);
            return std::unexpected(ErrorMessage(Core::Format("glfwGetRequiredInstanceExtensions failed: {}",
                                                             Desc ? Desc : "GLFW not initialized or no Vulkan support")));
        }
        for (auto* GlfwExt : std::span(GlfwExts, GlfwCount)) {
            auto It = std::ranges::find_if(m_InstanceExtensions,
                                           [&](const auto& Cap) { return Cap->Name() && std::strcmp(Cap->Name(), GlfwExt) == 0; });
            if (It != m_InstanceExtensions.end())
                (*It)->SetEnabled(false);  // re-enabled below if found in driver
            else
                m_InstanceExtensions.push_back(MakeCap(GlfwExt, true));
        }

        // Enumerate available instance extensions
        auto ExtPropsRes = Ctx.enumerateInstanceExtensionProperties();
        if (ExtPropsRes.result != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage("Failed to enumerate Vulkan instance extension properties"));
        auto& ExtProps = ExtPropsRes.value;

        for (auto& Prop : ExtProps)
            LogDebug("Available Vulkan instance extension: {}", static_cast<const char*>(Prop.extensionName));

        // Build flat list for match: instance extensions (void entries)
        std::vector<CapabilityNode*> Flat;
        Flat.reserve(m_InstanceExtensions.size());
        for (auto& Cap : m_InstanceExtensions) Flat.push_back(Cap.get());
        return MatchExtensions(Flat, ExtProps);
    }

    // ── Phase 2: Resolve device capabilities ─────────────────────────────

    [[nodiscard]] auto ResolveDeviceExtensionsAndFeatures(vk::raii::PhysicalDevice& PD)
        -> std::expected<std::tuple<std::vector<const char*>, const vk::PhysicalDeviceFeatures2&>, ErrorMessage> {
        // Enumerate available device extensions
        auto ExtPropsRes = PD.enumerateDeviceExtensionProperties();
        if (ExtPropsRes.result != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage("Failed to enumerate device extension properties"));
        auto& ExtProps = ExtPropsRes.value;
        for (auto& Prop : ExtProps)
            LogDebug("Available device extension: {}", static_cast<const char*>(Prop.extensionName));

        // Flat list for extension match
        std::vector<CapabilityNode*> Flat;
        Flat.reserve(m_DeviceCaps.size());
        for (auto& Cap : m_DeviceCaps) Flat.push_back(Cap.get());

        auto Result = MatchExtensions(Flat, ExtProps);
        if (!Result)
            return std::unexpected(Result.error());
        m_EnabledDeviceNames = std::move(*Result);

        // ── Merge same-sType feature entries ─────────────────────────────
        // Group by sType, OR feature bits together
        m_MergedFeatureEntries.clear();
        std::unordered_map<vk::StructureType, bool> MergedActive;

        for (auto& Cap : m_DeviceCaps) {
            if (!Cap->GetPNext()) continue;  // extension-only, skip

            auto SType       = Cap->GetSType();
            bool CapActive   = Cap->Name() == nullptr || Cap->Enabled();

            auto It = m_MergedFeatureEntries.find(SType);
            if (It == m_MergedFeatureEntries.end()) {
                auto Clone = Cap->Clone();
                m_MergedFeatureEntries.emplace(SType, std::move(Clone));
                MergedActive[SType] = CapActive;
            } else {
                // Merge feature bits — same sType means same C++ type
                // (guaranteed by Vulkan spec).  Virtual dispatch handles
                // the typed OrFeatures internally.
                It->second->MergeWith(*Cap);
                MergedActive[SType] = MergedActive[SType] || CapActive;
            }
        }

        // ── Build pNext chain from active merged entries ────────────────
        m_Features2 = vk::PhysicalDeviceFeatures2{};
        void* TailPNext = nullptr;

        // Collect active, iterate reverse so first registered = innermost chain
        std::vector<UPtr<CapabilityNode>*> Sorted;
        Sorted.reserve(m_MergedFeatureEntries.size());
        for (auto& [SType, Entry] : m_MergedFeatureEntries)
            if (MergedActive[SType])
                Sorted.push_back(&Entry);

        for (auto It = Sorted.rbegin(); It != Sorted.rend(); ++It) {
            void* Raw = const_cast<void*>((**It)->GetPNext());
            static_cast<PNextAccess*>(Raw)->pNext = TailPNext;
            TailPNext = Raw;
        }
        const_cast<PNextAccess*>(reinterpret_cast<const PNextAccess*>(&m_Features2))->pNext = TailPNext;

        return std::tuple<std::vector<const char*>, const vk::PhysicalDeviceFeatures2&>{
            m_EnabledDeviceNames, m_Features2};
    }

    // ── Queries ──────────────────────────────────────────────────────────

    [[nodiscard]] auto IsInstanceExtensionEnabled(const char* Name) -> bool {
        auto It = std::ranges::find_if(m_InstanceExtensions,
                                       [&](const auto& Cap) { return Cap->Name() && std::strcmp(Cap->Name(), Name) == 0; });
        return It != m_InstanceExtensions.end() && (*It)->Enabled();
    }

    [[nodiscard]] auto IsDeviceExtensionEnabled(const char* Name) -> bool {
        auto It = std::ranges::find_if(m_DeviceCaps,
                                       [&](const auto& Cap) { return Cap->Name() && std::strcmp(Cap->Name(), Name) == 0; });
        return It != m_DeviceCaps.end() && (*It)->Enabled();
    }

  private:
    // ── Shared match logic ───────────────────────────────────────────────

    [[nodiscard]] auto MatchExtensions(
        std::span<CapabilityNode*> Extensions,
        std::span<const vk::ExtensionProperties> Available) -> std::expected<std::vector<const char*>, ErrorMessage> {
        for (auto* Ext : Extensions) {
            if (Ext->Name())
                Ext->SetEnabled(false);
        }
        for (auto* Ext : Extensions) {
            auto* Name = Ext->Name();
            if (!Name) continue;
            if (std::ranges::any_of(Available,
                                    [&](const auto& Prop) { return std::strcmp(Prop.extensionName, Name) == 0; }))
                Ext->SetEnabled(true);
        }
        std::vector<StringView> Missing;
        for (const auto* Ext : Extensions) {
            if (!Ext->Name()) continue;
            if (Ext->Required() && !Ext->Enabled())
                Missing.emplace_back(Ext->Name());
        }
        if (!Missing.empty()) {
            String Msg;
            for (size_t i = 0; i < Missing.size(); ++i) {
                if (i > 0) Msg += ", ";
                Msg += Missing[i];
            }
            return std::unexpected(ErrorMessage(Core::Format("Required Vulkan extensions not supported: {}", Msg)));
        }
        std::vector<const char*> EnabledNames;
        for (const auto* Ext : Extensions) {
            if (Ext->Name() && Ext->Enabled())
                EnabledNames.push_back(Ext->Name());
        }
        return EnabledNames;
    }

    // ── Registration ────────────────────────────────────────────────────

    auto RegisterInstanceExtensions() -> void {
        m_InstanceExtensions.push_back(MakeCap(vk::KHRPortabilityEnumerationExtensionName, false));
    }

    auto RegisterDeviceCapabilities() -> void {
        // shaderDrawParameters — Slang unconditionally emits DrawParameters
        // SPIR-V capability into compiled shaders, even ones that don't use
        // gl_DrawID / gl_BaseInstance / gl_BaseVertex.  Core in Vulkan 1.1,
        // exposed via VK_KHR_shader_draw_parameters on older drivers.
        m_DeviceCaps.push_back(MakeCap(vk::KHRShaderDrawParametersExtensionName, false,
            vk::PhysicalDeviceVulkan11Features{.shaderDrawParameters = true}));
        // timeline semaphore support — core in Vulkan 1.2, exposed via
        // VK_KHR_timeline_semaphore on older drivers.
        m_DeviceCaps.push_back(MakeCap(vk::KHRTimelineSemaphoreExtensionName, false,
            vk::PhysicalDeviceVulkan12Features{.timelineSemaphore = true}));
        // VK_EXT_descriptor_indexing enables fully bindless resource access.
        // Required for: descriptor arrays, UPDATE_AFTER_BIND_BIT,
        // PARTIALLY_BOUND_BIT, variable descriptor counts.
        m_DeviceCaps.push_back(MakeCap(vk::EXTDescriptorIndexingExtensionName, true,
            vk::PhysicalDeviceVulkan12Features{
                .descriptorIndexing = true,
                .shaderUniformBufferArrayNonUniformIndexing = true,
                .shaderSampledImageArrayNonUniformIndexing = true,
                .shaderStorageBufferArrayNonUniformIndexing = true,
                .descriptorBindingUniformBufferUpdateAfterBind = true,
                .descriptorBindingSampledImageUpdateAfterBind = true,
                .descriptorBindingStorageBufferUpdateAfterBind = true,
                .descriptorBindingPartiallyBound = true,
                .descriptorBindingVariableDescriptorCount = true,
                .runtimeDescriptorArray = true,
            }));
        // synchronization2 — core in Vulkan 1.3, VK_KHR_synchronization2 on older.
        m_DeviceCaps.push_back(MakeCap(vk::KHRSynchronization2ExtensionName, false,
            vk::PhysicalDeviceVulkan13Features{.synchronization2 = true}));
        // dynamic rendering — core in Vulkan 1.3, VK_KHR_dynamic_rendering on older.
        m_DeviceCaps.push_back(MakeCap(vk::KHRDynamicRenderingExtensionName, false,
            vk::PhysicalDeviceVulkan13Features{.dynamicRendering = true}));
        // VK_KHR_swapchain — required for presenting rendered images to the window
        m_DeviceCaps.push_back(MakeCap(vk::KHRSwapchainExtensionName, true));
        // VK_KHR_portability_subset — needed for MoltenVK on Apple platforms
        m_DeviceCaps.push_back(MakeCap(vk::KHRPortabilitySubsetExtensionName, false));
        // VK_EXT_memory_budget — used by VMA; provides query for current
        // memory usage and budget
        m_DeviceCaps.push_back(MakeCap(vk::EXTMemoryBudgetExtensionName, false));
    }

    // ── Members ─────────────────────────────────────────────────────────

    std::vector<UPtr<CapabilityNode>> m_InstanceExtensions;
    std::vector<UPtr<CapabilityNode>> m_DeviceCaps;
    std::vector<const char*>          m_EnabledDeviceNames;
    vk::PhysicalDeviceFeatures2       m_Features2{};
    std::unordered_map<vk::StructureType, UPtr<CapabilityNode>> m_MergedFeatureEntries;
};

} // namespace SoulEngine::RHI::Vulkan
