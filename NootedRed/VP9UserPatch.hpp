#pragma once

#include <Headers/kern_patcher.hpp>
#include <libkern/OSTypes.h>

// Installs the in-place VP9 decode backend (into unused AV1 processor space in AMDRadeonVADriver2)
// and VideoToolbox decoder admission fix via private COW shared-cache transactions on macOS 13–26.
// Safely stays disabled when -NRedNoVP9 / -NRedNoVDA is passed.
class VP9UserPatch
{
public:
    static VP9UserPatch& singleton();

    void init();
    void processPatcher(KernelPatcher& patcher);

    bool isEnabled() const { return this->enabled; }

private:
    bool enabled{false};
};
