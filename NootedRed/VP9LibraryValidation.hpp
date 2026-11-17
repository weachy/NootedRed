// Optional, exact-cdhash library validation support for the paired VP9 decoder.
#pragma once
#include <Headers/kern_patcher.hpp>

namespace VP9LibraryValidation {
    void init();
    void processKext(KernelPatcher& patcher, size_t index, mach_vm_address_t address, size_t size);
}
