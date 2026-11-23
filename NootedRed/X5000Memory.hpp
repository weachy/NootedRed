#pragma once

#include <Headers/kern_patcher.hpp>

namespace X5000Memory
{
    void init();
    bool requested();
    // True when unified memory installs on this AMDRadeonX5000 image: requested, IOAcceleratorFamily2 verified, a
    // Renoir-family device and a supported X5000 build. X5000 then keeps its native VRAM allocator.
    bool managesX5000(mach_vm_address_t slide, size_t size);
    void processKext(KernelPatcher& patcher, size_t id, mach_vm_address_t slide, size_t size);
    void patchMetalBudgetPage(const char* file, UInt64 offset, const void* page);
    void patchGLBudgetPage(UInt64 offset, const void* page);
}
