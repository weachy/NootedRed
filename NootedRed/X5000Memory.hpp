// X5000 Unified Memory interface (see docs/X5000-UnifiedMemory.md)

#pragma once

#include <Headers/kern_patcher.hpp>
#include <UserPatches.hpp>

namespace X5000Memory
{

    void init();
    bool requested();
    bool installed();
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
