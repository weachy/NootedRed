// X5000 Unified Memory interface (see docs/X5000-UnifiedMemory.md)

#pragma once

#include <Headers/kern_patcher.hpp>
#include <UserPatches.hpp>

namespace X5000Memory
{

    void init();
    bool requested();
    // True when unified memory installs on this AMDRadeonX5000 image: requested, IOAcceleratorFamily2 verified, a
    // Renoir-family device and a supported X5000 build. X5000 then keeps its native VRAM allocator.
    bool managesX5000(mach_vm_address_t slide, size_t size);
    void processKext(KernelPatcher& patcher, size_t id, mach_vm_address_t slide, size_t size);

    // Budget sites in the user-space Metal driver (dyld shared cache) and OpenGL driver. UserPatches applies them
    // with its signature matcher; the list is empty until the kernel routes are installed.
    struct UserSite
    {
        bool                        sharedCache;
        UserPatches::SignaturePatch patch;
    };
    const UserSite* userBudgetSites(size_t& count);
    void            userBudgetSiteApplied(size_t index);

}    // namespace X5000Memory
