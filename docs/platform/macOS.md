# macOS Vulkan Driver Setup

Soul Engine uses Vulkan for graphics rendering on macOS. There are currently two available Vulkan ICDs (Installable Client Drivers) on the platform: **MoltenVK** and **KosmicKrisp**. Both map the Vulkan API down to Apple Metal, but they differ significantly in architecture, spec conformance, and future direction.

---

## Driver Comparison

| Feature | MoltenVK | KosmicKrisp |
|---------|----------|-------------|
| **Project Owner** | The Brenwill Workshop / Khronos Group | LunarG (funded by Google) |
| **Approach** | Vulkan Portability Extension translation layer | Native driver inside Mesa 3D |
| **Vulkan Version** | Subset via Portability Extension | **Vulkan 1.3 Conformant** (Khronos CTS passed) |
| **Apple Silicon** | Supported | **Supported (M1 / M2 / M3 / M4)** |
| **Intel Mac** | Supported | Not supported |
| **Status** | Mature and stable | Actively developed; the future direction |

### MoltenVK

MoltenVK is the long-standing Vulkan solution for macOS and iOS. It implements the **Vulkan Portability Extension** (`VK_KHR_portability_subset`) by translating Vulkan calls into Metal API calls. This means:

- Applications must explicitly request the Portability Subset; some Vulkan features are unavailable.
- It supports both Intel Macs and Apple Silicon.
- The ecosystem is mature and battle-tested across many projects.

### KosmicKrisp

KosmicKrisp is a next-generation driver introduced by LunarG in 2025, built directly on top of **Mesa 3D**. The key differences from MoltenVK are:

- **Native Vulkan 1.3 Conformance**: Passed the Khronos Conformance Test Suite (CTS) in late 2025. It is a full spec implementation, not a portability subset.
- **No Portability Extension Required**: Applications do not need to request `VK_KHR_portability_enumeration`, keeping logic simpler and cleaner.
- **Built on Metal 4**: Leverages the Apple Metal 4 foundation for better utilization of Apple Silicon GPU architecture.
- **Active Roadmap**: Planned 2026 deliverables include iOS support, Vulkan 1.4 conformance, and RenderDoc debugging support.

> See also: [LunarG Announcement](https://www.lunarg.com/lunarg-achieves-vulkan-1-3-conformance-with-kosmickrisp-on-apple-silicon/) | [Phoronix Coverage](https://www.phoronix.com/news/KosmicKrisp-Vulkan-1-3) | [Mesa Documentation](https://docs.mesa3d.org/drivers/kosmickrisp.html)

---

## Recommended Configuration

**Use KosmicKrisp as the preferred ICD.**

On Apple Silicon hardware, KosmicKrisp provides a cleaner native Vulkan path that avoids the functional limitations and extra enumeration overhead of the Portability Subset. It is the long-term direction for Vulkan on macOS.

If your application still needs to support Intel Macs, keep MoltenVK as a fallback. For Apple-Silicon-only deployments, switching entirely to KosmicKrisp is recommended.

### Switching Drivers in Vulkan Configurator

Open **Vulkan Configurator** (installed with the Vulkan SDK) and go to the **Vulkan Drivers** tab:

1. Check **Configure System Vulkan Devices**.
2. Set **Vulkan Devices override mode** to `Order Vulkan physical devices`.
3. Drag **KosmicKrisp** to the top of the list (First Physical Device) so it takes priority over MoltenVK.

The resulting configuration should look like this:

![Vulkan Configurator - macOS driver priority](macOS-vkconfig.png)

> The screenshot shows two ICDs enumerated on the system:
> - `KosmicKrisp Apple M4 (26.1.99)` — placed first
> - `MoltenVK Apple M4 (0.2.2209)` — placed second

### Force a Specific Driver from the Command Line

If you need to force KosmicKrisp in a terminal or CI environment:

```bash
export VK_DRIVER_FILES=$VULKAN_SDK/share/vulkan/icd.d/libkosmickrisp_icd.json
```

---

## Environment Requirements

- **macOS 15 (Sequoia)** or later (for the KosmicKrisp SDK-integrated build)
- **Apple Silicon** (M1 / M2 / M3 / M4)
- **Vulkan SDK 1.4.335+** (the first SDK release to bundle KosmicKrisp)

---

## Summary

| Scenario | Recommended Driver |
|----------|-------------------|
| Apple Silicon (M1/M2/M3/M4) development / runtime | **KosmicKrisp** |
| Intel Mac compatibility required | MoltenVK |
| Full Vulkan spec conformance and future extensibility | **KosmicKrisp** |
| Existing large codebase relying on Portability Subset | MoltenVK (migrate over time) |

Soul Engine targets **KosmicKrisp** by default on macOS. The RHI implementation will be built against native Vulkan 1.3 and will not introduce Portability Subset dependencies.
