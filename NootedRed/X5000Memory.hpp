#pragma once

#include <Headers/kern_patcher.hpp>

namespace X5000Memory
{
    void init();
    bool requested();
    void processKext(KernelPatcher& patcher, size_t id, mach_vm_address_t slide, size_t size);
    void patchMetalBudgetPage(const char* file, UInt64 offset, const void* page);
    void patchGLBudgetPage(UInt64 offset, const void* page);
}
