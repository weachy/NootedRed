#include <CompatTarget.hpp>
#include <GLResourceBudgetPatch.hpp>
#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <Kexts.hpp>
#include <NRed.hpp>
#include <X5000Memory.hpp>
#include <X5000MemoryPolicy.hpp>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSString.h>
#include <kern/task.h>
#include <sys/sysctl.h>

SYSCTL_DECL(_debug);
extern "C" char osversion[];

namespace
{
    using X5000Memory::BackingKind;
    using X5000Memory::Residency;
    using X5000Memory::ResourceKind;

    constexpr UInt8 ioAccelUUID[] = {0x6C, 0x4A, 0x16, 0xB1, 0x66, 0xDC, 0x3A, 0x8B,
                                    0x9F, 0x0B, 0xD3, 0xE0, 0x27, 0xB1, 0x6E, 0xB1};
    constexpr UInt8 x5000UUID[] = {0xE2, 0x7E, 0xFB, 0x64, 0xDE, 0x35, 0x3C, 0xE5,
                                  0xB3, 0xAB, 0xA3, 0x22, 0x18, 0xB4, 0x95, 0xD3};

    constexpr char allocPrimarySymbol[] = "__ZN16IOAccelResource211allocMemoryEP14IOAccelShared2";
    constexpr char allocDataSymbol[] = "__ZN16IOAccelResource221allocDataBufferMemoryEP14IOAccelShared2bj";
    constexpr char sysFactorySymbol[] =
        "__ZN16IOAccelSysMemory11withOptionsEP22IOGraphicsAccelerator2P4taskP14IOAccelShared2P16IOAccelResource2jyb";
    constexpr char poolFactorySymbol[] = "__ZN34IOAccelBufferMemoryDescriptorPool229newWiredDescriptorsWithLengthEyPjP4taskP12IOAccelEventbPb";
    constexpr char acceleratorStartSymbol[] = "__ZN22IOGraphicsAccelerator25startEP9IOService";
    constexpr char poolNewSymbol[] = "__ZN34IOAccelBufferMemoryDescriptorPool2nwEm";
    constexpr char poolInitSymbol[] = "__ZN34IOAccelBufferMemoryDescriptorPool219initWithAcceleratorEP22IOGraphicsAccelerator2";
    constexpr char poolReleaseSymbol[] =
        "__ZN34IOAccelBufferMemoryDescriptorPool218releaseDescriptorsEPP24IOBufferMemoryDescriptorjP4taskP12IOAccelEvent";
    constexpr char poolKickSymbol[] = "__ZN22IOGraphicsAccelerator219kickMemoryPoolTimerEv";
    constexpr char eventMachineSymbol[] = "__ZNK13IOAccelMemory15getEventMachineEv";
    constexpr char mergeEventsSymbol[] = "__ZN13IOAccelMemory20merge_mapping_eventsEP12IOAccelEvent";
    constexpr char sharedReleaseSymbol[] = "__ZN25IOAccelOrphanedMemoryPool13sharedReleaseEP14IOAccelShared2";
    constexpr char memoryReleaseSymbol[] = "__ZNK13IOAccelMemory7releaseEv";
    constexpr char pteUpdateSymbol[] = "__ZN29AMDRadeonX5000_AMDHWVMContext36updateContiguousPTEsWithDMAUsingAddrEyyyyy";
    constexpr char mapVMPTSymbol[] = "__ZN29AMDRadeonX5000_AMDHWVMContext7mapVMPTEP12AMD_VMPT_CTL15eAMD_VMPT_LEVELjyyy";
    constexpr char mapVASymbol[] =
        "__ZN29AMDRadeonX5000_AMDHWVMContext5mapVAEyP13IOAccelMemoryyyN24AMDRadeonX5000_IAMDHWVMM10VmMapFlagsE";
    constexpr char poolReserveSymbol[] = "__ZN35AMDRadeonX5000_AMDCommandBufferPool17getBufferStartPtrEj";
    constexpr char poolCommitSymbol[] = "__ZN35AMDRadeonX5000_AMDCommandBufferPool15setBufferEndPtrEPj";
    constexpr char poolSubmitSymbol[] = "__ZN35AMDRadeonX5000_AMDCommandBufferPool12submitBufferEy";
    constexpr char clientBufferSymbol[] =
        "__ZN16IOAccelSysMemory16withClientBufferEP22IOGraphicsAccelerator2P4taskP14IOAccelShared2yy";
    constexpr char freeSymbol[] = "__ZN16IOAccelSysMemory4freeEv";
    constexpr char wiredLimitSymbol[] = "__ZN22IOGraphicsAccelerator224getMaxWiredSysMemorySizeEv";
    constexpr char singleLimitSymbol[] = "__ZN22IOGraphicsAccelerator225getMaxSingleSysMemorySizeEv";
    constexpr char configSymbol[] = "__ZN14IOAccelDevice210get_configEP23IOAccelDeviceConfigData";
    constexpr char wiredSymbol[] = "__ZN22IOGraphicsAccelerator212sysmem_wiredEP16IOAccelSysMemory";
    constexpr char unwiredSymbol[] = "__ZN22IOGraphicsAccelerator214sysmem_unwiredEP16IOAccelSysMemory";
    constexpr char purgeRequestSymbol[] =
        "__ZN24IOAccelSharedUserClient222set_resource_purgeableEj25eIOAccelResourcePurgeablePS0_";
    constexpr char resourcePurgeableSymbol[] = "__ZN16IOAccelResource212setPurgeableEjjPj";
    constexpr char memoryPurgeableSymbol[] = "__ZN16IOAccelSysMemory12setPurgeableEjPj";
    constexpr char prepareCountSymbol[] = "__ZNK13IOAccelMemory15getPrepareCountEv";
    constexpr char testEventsSymbol[] = "__ZN13IOAccelMemory12testAllEventEv";
    constexpr char unwireSymbol[] = "__ZN16IOAccelSysMemory6unwireEv";
    constexpr char setFlagsSymbol[] = "__ZN13IOAccelMemory8setFlagsEj";
    constexpr char mapReleaseSymbol[] = "__ZNK16IOAccelMemoryMap7releaseEv";
    constexpr char orphanSymbol[] = "__ZN16IOAccelSysMemory8orphanItEv";
    constexpr char pruneSymbol[] = "__ZN13IOAccelMemory21pruneOrphanedMappingsEv";
    constexpr char mapAtSymbol[] = "__ZN13IOAccelMemory34createMappingInTaskAtAddressLengthEP11IOAccelTaskjyy";
    constexpr char taskSymbol[] = "__ZNK16IOAccelResource210getGPUTaskEv";
    constexpr char storageSymbol[] = "__ZN16IOAccelResource218getStorageResourceEv";
    constexpr char sharedInitSymbol[] = "__ZN14IOAccelShared24initEP22IOGraphicsAccelerator2P4task";
    constexpr char allocationParametersSymbol[] = "__ZN31AMDRadeonX5000_AMDAccelResource23getMemoryAllocParameterEv";
    constexpr char sysInitSymbol[] = "__ZN32AMDRadeonX5000_AMDAccelSysMemory4initEP22IOGraphicsAccelerator2";
    constexpr char mapInitSymbol[] = "__ZN32AMDRadeonX5000_AMDAccelMemoryMap4initEP22IOGraphicsAccelerator2P11IOAccelTaskP13IOAccelMemoryj";
    constexpr char mapFreeSymbol[] = "__ZN32AMDRadeonX5000_AMDAccelMemoryMap4freeEv";
    constexpr char commitSymbol[] = "__ZN32AMDRadeonX5000_AMDAccelMemoryMap22commitIntoGPUPageTableEv";
    constexpr char releasePTESymbol[] = "__ZN32AMDRadeonX5000_AMDAccelMemoryMap23releaseFromGPUPageTableEv";
    constexpr char surfaceLayoutSymbol[] = "__ZN31AMDRadeonX5000_AMDAccelResource38calculateIOSurfaceDeviceCacheVRAMBytesEPyS0_";
    constexpr char flipSymbol[] =
        "__ZN27AMDRadeonX5000_AMDHWDisplay29getDisplayPipeTransactionFlipEjP16IOAccelResource2S1_17_eAMDSwapInterval"
        "P14_AMD_PIPE_FLIPb42AMDAccelDisplayPipeTransactionPipeRotationS4_";


    constexpr size_t resourceFlags = 0x0C;
    constexpr size_t resourceType = 0x14;
    constexpr size_t resourceAccelerator = 0x18;
    constexpr size_t backingMemory = 0x80;
    constexpr size_t primaryMemory = 0x88;
    constexpr size_t resourceSurface = 0xE0;
    constexpr size_t dataBufferFlags = 0xF4;
    constexpr UInt8 dataBufferNativePlacement = 1;
    constexpr size_t surfaceLayout = 0x180;
    constexpr size_t resourceCopyFlags = 0x1B4;
    constexpr size_t surfaceFlags = 0x1B8;
    constexpr size_t allocationAlignment = 0x218;
    constexpr size_t maskStorage[] = {0x188, 0x190, 0x198, 0x1A0};
    constexpr UInt32 specialResource = (1U << 26) | (1U << 30) | (1U << 31);
    constexpr UInt8 surfaceInitialized = 0x08;
    constexpr UInt8 displayAddress = 0x20;
    constexpr UInt8 externalSurfaceLayout = 0x80;
    constexpr UInt32 copyKindMask = 0xFU << 26;
    constexpr UInt32 copyKindLinear = 4U << 26;
    constexpr UInt32 copyFormatMask = 0xFF;
    constexpr size_t memoryFlags = 0x0C;
    constexpr UInt8 clientBufferMemory = 0x40;
    constexpr UInt32 wiredMemory = 1U << 1;
    constexpr UInt32 pooledMemory = 1U << 13;
    constexpr UInt32 purgeableShift = 7;
    constexpr UInt32 purgeableMask = 7;
    constexpr size_t memorySize = 0x40;
    constexpr size_t memoryOptionsOffset = 0xD8;
    constexpr size_t sysMemoryAlignment = 0x1A0;
    constexpr size_t acceleratorWiredLimit = 0x348;
    constexpr size_t acceleratorWiredBytes = 0x350;
    constexpr size_t deviceAccelerator = 0x108;
    constexpr size_t configTextureBudget = 0x08;
    constexpr size_t configWiredBudget = 0x20;
    constexpr UInt32 configSeparateWorkingSet = 1U << 31;
    constexpr size_t sharedTask = 0x88;
    constexpr size_t mappingMemory = 0x18;
    constexpr size_t mappingAccelerator = 0x88;
    constexpr size_t mappingTask = 0x90;
    constexpr size_t mappingGPUAddress = 0x98;
    constexpr size_t taskPageTable = 0x260;
    constexpr size_t taskFlags = 0x268;
    constexpr UInt8 perTaskPageTable = 1;


    constexpr size_t primaryGateOffset = 0x27;
    constexpr size_t primaryNativeSystem = 0xDA;
    constexpr size_t primaryOptionsOffset = 0x161;
    constexpr size_t primaryFactoryCall = 0x186;
    constexpr size_t bufferFactoryArgsOffset = 0xBA;
    constexpr size_t bufferFactoryCall = 0xC4;
    constexpr size_t bufferCPUMappingOffset = 0xD5;
    constexpr size_t poolGateOffset = 0x87;
    constexpr size_t pageableBodyOffset = 0x165;
    constexpr size_t poolFactoryCall = 0x114;
    constexpr UInt8 primaryGate[] = {0x8B, 0x87, 0x90, 0x0C, 0, 0, 0x0F, 0xBA,
                                     0xE0, 0x15, 0x0F, 0x82, 0xA3, 0, 0, 0};
    constexpr UInt8 poolGate[] = {0x83, 0xBB, 0xBC, 0x09, 0, 0, 0, 0x0F, 0x84, 0xD1, 0, 0, 0, 0x31, 0xC0};
    // IOGraphicsAccelerator2::start creates the buffer pool only for drivers whose config marks system memory as
    // primary: testb $0x20, 0xc92(%r14); je skip; movl $0x1b0, %edi; callq operator new.
    constexpr size_t poolCreateGateOffset = 0x97E;
    constexpr size_t poolCreateCall = 0x98D;
    constexpr size_t poolCreateSkip = 0x9AF;
    constexpr UInt8 poolCreateGate[] = {0x41, 0xF6, 0x86, 0x92, 0x0C, 0, 0, 0x20, 0x74, 0x27, 0xBF, 0xB0, 0x01, 0, 0};
    static_assert(poolCreateGateOffset + sizeof(poolCreateGate) == poolCreateCall);
    static_assert(poolCreateGateOffset + 10 + 0x27 == poolCreateSkip);
    // IOAccelMemory flag 2 makes IOAccelMemoryMap::release keep a task's last mapping, with its GPU address and
    // committed page-table entries, on the task's orphan list instead of freeing it. createMappingInTaskAtAddressLength
    // revives it for the same task. setFlags: testb $0x2, %sil; je; orbs $0x10 into byte 0xd. release: movq 0x18(%rbx),
    // %rcx; testb $0x10, 0xd(%rcx); jne orphan.
    constexpr UInt32 keepOrphanedMappings = 2;
    constexpr UInt32 keptMappings = 1U << 12;
    constexpr UInt8 setFlagsKeepMappings[] = {0x40, 0xF6, 0xC6, 0x02, 0x74, 0x04, 0x80, 0x4F, 0x0D, 0x10};
    constexpr UInt8 mapReleaseKeepTest[] = {0x48, 0x8B, 0x4B, 0x18, 0xF6, 0x41, 0x0D, 0x10, 0x75, 0x37};
    // pruneOrphanedMappings walks the mapping array: movswq 0x5a(%rdi), %r15.
    constexpr size_t memoryMappingCount = 0x5A;
    constexpr UInt8 pruneMappingCount[] = {0x4C, 0x0F, 0xBF, 0x7F, 0x5A};

    // Native buffer pool. IOAccelSysMemory::orphanIt hands a pooled backing's descriptors back with
    // releaseDescriptors(pool = accel+0xc30 if accel+0x9bc, memory+0xf8, memory+0x100, *(shared+0x80) or kernel_task,
    // merged mapping events) and then clears flag 0x800, so the later unwire does not mark them Volatile.
    constexpr size_t memoryAccelerator = 0x28;
    constexpr size_t memoryShared = 0x30;
    constexpr size_t memoryResource = 0x38;
    constexpr size_t memoryDescriptors = 0xF8;
    constexpr size_t memoryDescriptorCount = 0x100;
    constexpr size_t sharedPoolTask = 0x80;
    constexpr size_t acceleratorPoolEnabled = 0x9BC;
    constexpr size_t acceleratorPool = 0xC30;
    constexpr UInt32 orphanedPurgeable = 1U << 11;
    constexpr UInt32 nonVolatileState = 2;
    constexpr UInt32 volatileState = 3;
    // Slots are offsets from the object's vptr (vtable symbol + 0x10). Verified live through an AMDAccelSysMemory
    // vptr: 0x1c0 orphanIt, 0x1d0 increment_wire_count (called from commit_pte+0x51), 0x1d8 decrement_wire_count
    // (release_pte+0x72); event machine 0x140 initEvent, 0x1b0 copyEvent, 0x1b8 mergeEvent.
    constexpr size_t incrementWireSlot = 0x1D0;
    constexpr size_t decrementWireSlot = 0x1D8;
    constexpr size_t eventInitSlot = 0x140;
    // merge_mapping_events and the pool's dirty hit call this slot as mergeEvent(machine, source, destination).
    constexpr size_t eventMergeSlot = 0x1B8;
    // AMDHWVMContext::getPageTableUpdateEvent returns context+0x40. After unmapVA returns it holds the VMPT queue
    // (software channel 15) stamp of the buffer that carries the PTE clear and the VM invalidation.
    constexpr size_t contextUpdateEvent = 0x40;
    constexpr size_t sharedAccelerator = 0x78;
    constexpr size_t poolDirtySlots = 0x110;
    constexpr size_t poolDirtyMask = 0x120;
    constexpr size_t poolDirtySlotBytes = 0x10;
    constexpr size_t poolDirtyEvents = 0x118;
    // IOAccelMemory::release: once only orphaned mappings are left and their events are done it retains, calls
    // pruneOrphanedMappings at +0xa6 and tail-calls release. This, not orphanIt, is how almost every pooled backing
    // ends; orphanIt only runs while mapping events are still pending.
    constexpr size_t releasePruneCall = 0xA6;
    constexpr UInt8 releasePruneTail[] = {0x48, 0x8B, 0x0B, 0x48, 0x83, 0xC1, 0x28};
    constexpr size_t eventBytes = 0x40;
    constexpr size_t poolBuckets = 5;
    constexpr size_t poolWiredCount = 0x28;
    constexpr size_t poolPurgeableCount = 0xA0;
    constexpr size_t poolBucketStride = 0x18;
    constexpr size_t poolDirtyCount = 0x128;
    constexpr size_t poolDirtyStride = 0x20;
    // initWithAccelerator reads (shift, capacity) per bucket from sWiredPoolInfo and sPurgeablePoolInfo through
    // leaq disp(%rip), %rcx and stores a fixed prefill of 24 (movabsq $0x1800000000). newWiredDescriptorsWithLength
    // caps a request at prefill << shift of the first bucket and gathers at most 64 descriptors in a stack array
    // (memset 0x200). With the native first bucket of 1 MiB the cap is 24 MiB.
    constexpr size_t poolInfoLoad = 0x6B;
    constexpr size_t poolPurgeableLoad = 0x127;
    constexpr size_t poolPrefillStore = 0x5C;
    constexpr UInt8 poolInfoLea[] = {0x48, 0x8D, 0x0D};
    constexpr UInt8 poolPrefill[] = {0x48, 0xB8, 0, 0, 0, 0, 0x18, 0, 0, 0};
    constexpr size_t poolCapCheck = 0x36;
    constexpr UInt8 poolCap[] = {0x8A, 0x4F, 0x34, 0x8B, 0x7F, 0x2C, 0xD3, 0xE7, 0x31, 0xC0, 0x48, 0x39, 0xFE};
    constexpr size_t poolStackClear = 0x62;
    constexpr UInt8 poolStack[] = {0x48, 0x8D, 0xBD, 0xD0, 0xFD, 0xFF, 0xFF, 0xBA, 0x00, 0x02, 0x00, 0x00};
    constexpr UInt32 nativeWiredInfo[15] = {20, 64, 24, 18, 32, 24, 16, 32, 24, 14, 32, 24, 12, 32, 24};
    constexpr UInt32 nativePurgeableInfo[15] = {20, 64, 0, 18, 16, 0, 16, 16, 0, 14, 16, 0, 12, 16, 0};
    // A 4 MiB first bucket raises the cap to 96 MiB (24 << 22), which covers WindowServer's 10-bit full-screen
    // textures (25-64 MiB). The worst case below the cap is 23 + 3 + 3 + 3 + 15 = 47 descriptors. The 16 KiB bucket
    // is dropped, so remainders below 64 KiB use 4 KiB pieces. Idle wired memory: 24-32 x 4 MiB in the first bucket.
    constexpr UInt32 largeWiredInfo[15] = {22, 32, 24, 20, 64, 24, 18, 32, 24, 16, 32, 24, 12, 32, 24};
    constexpr UInt32 largePurgeableInfo[15] = {22, 32, 0, 20, 64, 0, 18, 16, 0, 16, 16, 0, 12, 16, 0};
    constexpr UInt64 nativePoolLimit = 24ULL << 20;
    constexpr UInt8 nativeSystemHead[] = {0x49, 0x8B, 0x8E, 0xC8, 0, 0, 0};
    constexpr UInt8 primaryOptionsHead[] = {0x0F, 0xBA, 0xE0, 0x1E, 0xB8, 0x63, 0x03, 0, 0,
                                            0x41, 0xB8, 0x33, 0, 0, 0, 0x44, 0x0F, 0x43, 0xC0};
    constexpr UInt8 primaryPostFactory[] = {0x49, 0x89, 0xC4, 0x48, 0x85, 0xC0};
    constexpr UInt8 bufferFactoryArgs[] = {0x31, 0xF6, 0x4C, 0x89, 0xE2, 0x31, 0xC9, 0x45, 0x89, 0xF0};
    constexpr UInt8 bufferPostFactory[] = {0x48, 0x85, 0xC0, 0x74, 0x4E, 0x49, 0x89, 0xC5, 0x80, 0x48, 0x0C, 0x01};
    constexpr UInt8 bufferCPUMappingGate[] = {0xF6, 0x83, 0xF4, 0, 0, 0, 0x01, 0x75, 0x6E};
    static_assert(bufferFactoryArgsOffset + sizeof(bufferFactoryArgs) == bufferFactoryCall);
    constexpr size_t purgeRequestCall = 0xFD;
    constexpr UInt8 purgeRequestArgs[] = {0x48, 0x89, 0xC7, 0x44, 0x89, 0xFE, 0x44, 0x89, 0xFA};
    constexpr UInt8 purgeablePrimaryLoad[] = {0x48, 0x8B, 0xBB, 0x88, 0, 0, 0};
    constexpr UInt8 purgeableFlagsLoad[] = {0x41, 0x8B, 0x46, 0x0C, 0x89, 0xC1, 0xF7, 0xD1};
    constexpr UInt8 memoryOptionsStore[] = {0x41, 0x89, 0x85, 0xD8, 0, 0, 0};
    constexpr UInt8 prepareCountHead[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x50, 0x8B, 0x5F, 0x10};
    constexpr UInt8 testEventsHead[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x56, 0x53, 0x48, 0x89, 0xFB, 0x66, 0x83, 0x7F, 0x5A, 0};
    constexpr UInt8 unwireClearsWired[] = {0x80, 0x63, 0x0C, 0xFD, 0x48, 0x8B, 0x7B, 0x28};
    constexpr UInt8 pageableHead[] = {0x31, 0xC0, 0x48, 0x89, 0x45, 0xA0};
    constexpr UInt8 wiredLimitBody[] = {0x55, 0x48, 0x89, 0xE5, 0x48, 0x8B, 0x87, 0x48, 0x03, 0, 0, 0x5D, 0xC3, 0x90};
    constexpr UInt8 singleLimitHead[] = {0x55, 0x48, 0x89, 0xE5, 0x48, 0x8B, 0x87, 0x48, 0x03, 0, 0, 0x48, 0x8D, 0x0C, 0x40};
    constexpr UInt8 taskHead[] = {0x55, 0x48, 0x89, 0xE5, 0x48, 0x8B, 0x47, 0x68, 0x48, 0x85, 0xC0};
    constexpr UInt8 surfaceOwnerLoad[] = {0x48, 0x8B, 0x9B, 0xE0, 0, 0, 0};
    constexpr UInt8 sharedTaskStore[] = {0x48, 0x89, 0x83, 0x88, 0, 0, 0, 0x48, 0x85, 0xC0};
    constexpr UInt8 clientBufferFlagStore[] = {0x80, 0x4B, 0x0C, 0x40};
    constexpr UInt8 configAcceleratorLoad[] = {0x4C, 0x8B, 0xA7, 0x08, 0x01, 0, 0};
    constexpr UInt8 configBitsStore[] = {0x0F, 0xB7, 0xC8, 0xC1, 0xE8, 0x09, 0x25, 0x00, 0x10, 0, 0,
                                         0x09, 0xC8, 0x89, 0x03};
    constexpr UInt8 configTextureStore[] = {0xFF, 0x90, 0x68, 0x08, 0, 0, 0x48, 0x89, 0x43, 0x08};
    constexpr UInt8 configWiredStore[] = {0xFF, 0x90, 0x90, 0x09, 0, 0, 0x48, 0x89, 0x43, 0x20};
    constexpr UInt8 nativeAlignmentField[] = {0x48, 0x81, 0xC3, 0x18, 0x02, 0, 0, 0x48, 0x89, 0xD8};
    constexpr UInt8 sysAlignmentInit[] = {0xC7, 0x87, 0xA0, 0x01, 0, 0, 0, 0, 0, 0};
    constexpr UInt8 sysAlignmentRead[] = {0x41, 0x8B, 0x87, 0xA0, 0x01, 0, 0, 0x48, 0x39, 0x83, 0x80, 0, 0, 0};
    constexpr UInt8 mappingTaskTest[] = {0x48, 0x8B, 0x87, 0x90, 0, 0, 0, 0xF6, 0x80, 0x68, 0x02, 0, 0, 0x01};
    constexpr UInt8 commitPageTableLoad[] = {0x4C, 0x8B, 0xB8, 0x60, 0x02, 0, 0, 0x48, 0x8B, 0x83, 0x98, 0, 0, 0};
    constexpr UInt8 releasePageTableLoad[] = {0x48, 0x8B, 0xB8, 0x60, 0x02, 0, 0, 0x48, 0x8D, 0x73, 0x38};
    constexpr UInt8 mapFreeHead[] = {0x55, 0x48, 0x89, 0xE5, 0x31, 0xC0, 0x48, 0x89, 0x87, 0x18, 0x01, 0, 0};
    constexpr UInt8 flipEligibility[] = {0x41, 0xF6, 0x87, 0xB8, 0x01, 0, 0, 0x20};
    constexpr UInt8 surfaceLayoutGate[] = {0x41, 0x80, 0xBE, 0xB8, 0x01, 0, 0, 0, 0x78, 0x2B,
                                           0x49, 0x8B, 0x86, 0x80, 0x01, 0, 0};

    struct MetalBudgetSite
    {
        const char* file;
        UInt64 pageOffset;
        size_t siteOffset;
        size_t length;
        UInt8 original[33];
        UInt8 replacement[33];
    };

    constexpr MetalBudgetSite workingSetSite{
        "dyld_shared_cache_x86_64h", 0xF5A1000, 0x19A, 33,
        {0x55, 0x48, 0x89, 0xE5, 0x48, 0x8B, 0x05, 0x73, 0xA0, 0x7B, 0x34, 0x48, 0x8B, 0x04, 0x07, 0x48,
         0x85, 0xC0, 0x75, 0x0B, 0x48, 0x8B, 0x05, 0x8B, 0xA0, 0x7B, 0x34, 0x48, 0x8B, 0x04, 0x07, 0x5D, 0xC3},
        {0x48, 0x8D, 0x8F, 0x20, 0x03, 0, 0, 0x80, 0x79, 0x03, 0, 0x78, 0x0E, 0x48, 0x8B, 0x41, 0x10,
         0x48, 0x85, 0xC0, 0x75, 0x04, 0x48, 0x8B, 0x41, 0x18, 0xC3, 0x48, 0x8B, 0x41, 0x08, 0xC3, 0x90},
    };
    constexpr MetalBudgetSite segmentSite{
        "dyld_shared_cache_x86_64h.03", 0x10C55000, 0xCD9, 28,
        {0x48, 0x8B, 0x05, 0, 0x19, 0x3C, 0x2F, 0x48, 0x8B, 0x00, 0x48, 0x8B, 0x04, 0x03,
         0x48, 0x8B, 0x0D, 0xE2, 0x18, 0x3C, 0x2F, 0x48, 0x8B, 0x09, 0x48, 0x89, 0x04, 0x0B},
        {0x48, 0x8D, 0x83, 0x20, 0x03, 0, 0, 0x48, 0x8B, 0x48, 0x10, 0x80, 0x78, 0x03, 0, 0x79, 0x07,
         0x48, 0x8B, 0x48, 0x08, 0x48, 0xD1, 0xE9, 0x48, 0x89, 0x48, 0x28},
    };
    static_assert(workingSetSite.siteOffset + workingSetSite.length <= PAGE_SIZE
                  && segmentSite.siteOffset + segmentSite.length <= PAGE_SIZE);

    // The OpenGL driver's resource list takes its budget from the device configuration (UnifiedReplace replaces
    // GLResourceBudgetPatch::Find at the same site), and the share group copies it into each context.
    namespace GLBudget
    {
        constexpr size_t ShareGroupPage = 0x1000;
        constexpr size_t ShareGroupOffset = 0x5CB;
        constexpr size_t ResourceListPage = 0x5000;
        constexpr size_t ResourceListOffset = 0x72A;
        constexpr size_t ConfigBudget = 0x1C;
        constexpr uint32_t ConfigCommandBudgetPresent = 1U << 30;

        constexpr uint8_t ShareGroupFind[] = {
            0xC5, 0xF8, 0x57, 0xC0, 0xC5, 0xFC, 0x11, 0x83, 0xFC, 0x01, 0x00, 0x00, 0xC5, 0xFC, 0x11, 0x83,
            0xE8, 0x01, 0x00, 0x00, 0xC5, 0xFC, 0x11, 0x83, 0xC8, 0x01, 0x00, 0x00, 0xC5, 0xFC, 0x11, 0x83,
            0xA8, 0x01, 0x00, 0x00, 0xC5, 0xFC, 0x11, 0x83, 0x88, 0x01, 0x00, 0x00, 0xC5, 0xFC, 0x11, 0x83,
            0x68, 0x01, 0x00, 0x00, 0x48, 0x8B, 0x43, 0x08, 0x48, 0x8B, 0xB8, 0x30, 0x02, 0x00, 0x00,
        };
        constexpr uint8_t ShareGroupReplace[] = {
            0x4C, 0x89, 0xFF, 0x31, 0xC0, 0xB9, 0x2D, 0x00, 0x00, 0x00, 0xF3, 0xAB, 0x48, 0x8B, 0x43, 0x08,
            0x48, 0x8B, 0xB8, 0x30, 0x02, 0x00, 0x00, 0x48, 0x85, 0xFF, 0x74, 0x13, 0xF6, 0x87, 0x73, 0x02,
            0x00, 0x00, 0x40, 0x74, 0x0A, 0x8B, 0x87, 0x8C, 0x02, 0x00, 0x00, 0x41, 0x89, 0x47, 0x54, 0x90,
            0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
        };
        constexpr uint8_t UnifiedReplace[] = {
            0x48, 0x8B, 0x96, 0xD8, 0x12, 0x00, 0x00, 0x0F, 0x57, 0xC0, 0x0F, 0x11, 0x07, 0x0F, 0x11, 0x47,
            0x18, 0x48, 0x89, 0x77, 0x10, 0x8B, 0x42, 0x54, 0x85, 0xC0, 0x75, 0x12, 0x8B, 0x42, 0x14, 0xC1,
            0xE0, 0x09, 0x8D, 0x8C, 0x00, 0x00, 0x00, 0xF8, 0xFF, 0x39, 0xC1, 0x0F, 0x4F, 0xC1, 0x89, 0x47,
            0x20, 0xC3,
        };
        static_assert(sizeof(ShareGroupFind) == 63 && sizeof(ShareGroupFind) == sizeof(ShareGroupReplace));
        static_assert(sizeof(UnifiedReplace) == sizeof(GLResourceBudgetPatch::Find));
    }

    bool enabledAtBoot{false};
    bool ioReady{false};
    bool amdReady{false};
    UInt32 active{0};
    UInt32 requestedBudgetMiB{0};
    const OSMetaClass* acceleratorClass{nullptr};
    const OSMetaClass* sysMemoryClass{nullptr};
    void* (*getGPUTask)(void*){nullptr};
    const void* (*getAllocationParameters)(void*){nullptr};
    UInt32 (*getPrepareCount)(const OSObject*){nullptr};
    bool (*allEventsComplete)(OSObject*){nullptr};
    void (*unwireMemory)(OSObject*){nullptr};
    void (*setMemoryFlags)(OSObject*, UInt32){nullptr};
    void (*pruneOrphanedMappings)(OSObject*){nullptr};
    void (*releasePoolDescriptors)(void*, void*, UInt32, void*, void*){nullptr};
    void (*kickPoolTimer)(IOService*){nullptr};
    void* (*getEventMachine)(const OSObject*){nullptr};
    void (*mergeMappingEvents)(OSObject*, void*){nullptr};
    bool largePoolInstalled{false};
    // Set only when the shared release and prune routes were verified in place; pool_return cannot be enabled before.
    bool poolReturnAvailable{false};
    // The prune collector reads the VMPT event from the routed releaseFromGPUPageTable; verified in installX5000.
    bool releasePTERouted{false};
    // On by default; each takes effect only when its routes were verified. Runtime off switches: sysctl.
    UInt32 poolReturnEnabled{1};
    UInt32 poolLargeEnabled{1};
    void* activePool{nullptr};
    mach_vm_address_t originalSharedRelease{0};
    mach_vm_address_t originalPrune{0};
    mach_vm_address_t releasePruneReturn{0};
    // Pool occupancy sampled under the accelerator lock (wired/purgeable/dirty per bucket), read by the sysctl.
    UInt32 poolSnapshot[poolBuckets][3]{};
    // VMPT events of the PTE releases done while a pooled backing's orphaned mappings are pruned (orphanBacking or
    // pruneFromRelease). Owned by the thread that holds the accelerator lock.
    thread_t pruneOwner{nullptr};
    void* pruneMachine{nullptr};
    IOService* pruneAccelerator{nullptr};
    alignas(8) UInt8 pruneEvent[eventBytes]{};
    mach_vm_address_t primaryFactoryReturn{0};
    mach_vm_address_t purgeRequestReturn{0};
    mach_vm_address_t originalDataAllocator{0};
    mach_vm_address_t originalSingleLimit{0};
    mach_vm_address_t originalGetConfig{0};
    mach_vm_address_t originalWired{0};
    mach_vm_address_t originalUnwired{0};
    mach_vm_address_t originalSetResourcePurgeable{0};
    mach_vm_address_t originalFree{0};
    mach_vm_address_t originalOrphan{0};
    mach_vm_address_t originalMapAt{0};
    mach_vm_address_t originalMapInit{0};
    mach_vm_address_t originalMapFree{0};
    mach_vm_address_t originalCommit{0};
    mach_vm_address_t originalReleasePTE{0};
    mach_vm_address_t originalFlip{0};

    UInt64 pageableRequests{0};
    UInt64 pooledAllocations{0};
    UInt64 poolReturns{0};
    UInt64 poolReturnPieces{0};
    UInt64 poolReturnSkips{0};
    UInt64 poolLargeRequests{0};
    UInt64 poolRekeyed{0};
    UInt64 allocationFailures{0};
    UInt64 bufferAllocations{0};
    UInt64 textureAllocations{0};
    UInt64 surfaceAllocations{0};
    UInt64 liveBytes{0};
    UInt64 wireCount{0};
    UInt64 unwireCount{0};
    UInt64 rewireCount{0};
    UInt64 purgeUnwires{0};
    UInt64 sameAddressRecommits{0};
    UInt64 sharedAddressRecommits{0};
    UInt64 mappingAddressChanges{0};
    UInt64 mappingTrackingFailures{0};
    UInt64 taskCommits{0};
    UInt64 unwiredBytes{0};
    UInt64 residentBytes{0};
    UInt64 totalResidentBytes{0};
    UInt64 deniedFlips{0};
    UInt64 effectiveBudget{0};
    bool metalSegmentReady{false};
    UInt64 metalWorkingSet{0};
    UInt64 metalBudgetPages{0};
    UInt64 metalBudgetQueries{0};
    UInt64 glBudgetKiB{0};
    UInt64 glBudgetSites{0};
    UInt64 glBudgetQueries{0};

    struct TaskMapping
    {
        const void* object;
        const void* task;
        TaskMapping* next;
        UInt64 address{0};
        UInt64 releasedAt{0};
        bool committed{false};
    };

    struct Backing
    {
        void* memory;
        Backing* next;
        UInt64 size;
        Residency residency;
        TaskMapping* mappings{nullptr};
        UInt64 evictionEpoch{0};
        bool poolReturned{false};
    };

    IOSimpleLock* backingLock{nullptr};
    Backing* backingBuckets[256]{};

    bool ready() { return __atomic_load_n(&active, __ATOMIC_ACQUIRE) != 0; }

    void increment(UInt64& counter, const UInt64 amount = 1)
    {
        __atomic_add_fetch(&counter, amount, __ATOMIC_RELAXED);
    }

    Backing*& bucketFor(void* const memory)
    {
        const auto address = reinterpret_cast<uintptr_t>(memory);
        const auto hash = (address >> 4) ^ (address >> 12);
        return backingBuckets[hash % arrsize(backingBuckets)];
    }

    Backing** findBacking(void* const memory)
    {
        auto** link = &bucketFor(memory);
        while (*link != nullptr && (*link)->memory != memory) { link = &(*link)->next; }
        return link;
    }

    TaskMapping** findMapping(Backing& backing, const void* const mapping)
    {
        auto** link = &backing.mappings;
        while (*link != nullptr && (*link)->object != mapping) { link = &(*link)->next; }
        return link;
    }

    bool hasOtherTask(const Backing& backing, const TaskMapping& mapping)
    {
        for (auto* peer = backing.mappings; peer != nullptr; peer = peer->next) {
            if (peer->address != 0 && peer->task != mapping.task) { return true; }
        }
        return false;
    }

    bool isManaged(void* const memory)
    {
        if (memory == nullptr) { return false; }
        IOSimpleLockLock(backingLock);
        const auto managed = *findBacking(memory) != nullptr;
        IOSimpleLockUnlock(backingLock);
        return managed;
    }

    void updateBackingResidency(void* const memory, const bool wired)
    {
        IOSimpleLockLock(backingLock);
        if (auto* const backing = *findBacking(memory)) {
            const auto change = X5000Memory::updateResidency(backing->residency, wired);
            backing->residency = change.state;
            if (change.wired) {
                increment(wireCount);
                increment(residentBytes, backing->size);
            }
            if (change.evicted) {
                ++backing->evictionEpoch;
                increment(unwireCount);
                increment(unwiredBytes, backing->size);
                __atomic_sub_fetch(&residentBytes, backing->size, __ATOMIC_RELAXED);
            }
            if (change.rewired) { increment(rewireCount); }
        }
        IOSimpleLockUnlock(backingLock);
    }

    bool ownsAccelerator(IOService* const accelerator)
    {
        if (!ready() || accelerator == nullptr) { return false; }
        const auto& nred = NRed::singleton();
        return nred.getIGPU() != nullptr && nred.getAttributes().isRenoir()
               && accelerator->metaCast(acceleratorClass) != nullptr && accelerator->getProvider() == nred.getIGPU();
    }

    bool hasPerTaskPageTable(void* const task)
    {
        return task != nullptr && (getMember<UInt8>(task, taskFlags) & perTaskPageTable) != 0
               && getMember<void*>(task, taskPageTable) != nullptr;
    }

    bool hasAuxiliaryStorage(void* const resource)
    {
        for (const auto offset : maskStorage) {
            if (getMember<void*>(resource, offset) != nullptr) { return true; }
        }
        return false;
    }

    BackingKind primaryBackingForResource(void* const resource, void* const shared)
    {
        if (resource == nullptr || !ownsAccelerator(getMember<IOService*>(resource, resourceAccelerator))) {
            return BackingKind::Native;
        }


        X5000Memory::Requirements usage{};
        usage.kind = static_cast<ResourceKind>(getMember<UInt8>(resource, resourceType));
        auto* const ownerTask = shared != nullptr ? getMember<void*>(shared, sharedTask) : nullptr;
        usage.usesTaskVM = hasPerTaskPageTable(ownerTask);
        usage.existingAllocation = getMember<void*>(resource, primaryMemory) != nullptr;
        usage.specialAddress = (getMember<UInt32>(resource, resourceFlags) & specialResource) != 0;
        if (usage.kind == ResourceKind::Texture || usage.kind == ResourceKind::Surface) {
            const auto attributes = getMember<UInt8>(resource, surfaceFlags);
            usage.importsSurface = getMember<void*>(resource, resourceSurface) != nullptr;
            usage.directDisplay = (attributes & displayAddress) != 0;
            usage.auxiliaryStorage = hasAuxiliaryStorage(resource);
            if (usage.kind == ResourceKind::Surface) {
                auto* const task = getGPUTask(resource);
                auto* const imported = getMember<OSObject*>(resource, backingMemory);
                const auto* const layout = getMember<void*>(resource, surfaceLayout);
                const bool systemBacking = imported != nullptr && imported->metaCast(sysMemoryClass) != nullptr;
                usage.usesTaskVM = hasPerTaskPageTable(task);
                usage.specialAddress |= (attributes & externalSurfaceLayout) != 0;
                usage.hasSurfaceCache = systemBacking && layout != nullptr && (attributes & surfaceInitialized) != 0;
                usage.hasLinearClientBuffer = systemBacking && layout == nullptr && attributes == 0 && task == ownerTask
                    && (getMember<UInt8>(imported, memoryFlags) & clientBufferMemory) != 0
                    && (getMember<UInt32>(resource, resourceCopyFlags) & (copyKindMask | copyFormatMask)) == copyKindLinear;
            }
        }
        return X5000Memory::choosePrimaryBacking(usage);
    }

    bool verifiedImage(const mach_vm_address_t start, const size_t length, const UInt8 (&expected)[16])
    {
        CompatTarget::Identity image{};
        return CompatTarget::parseMachOIdentity(image, reinterpret_cast<const UInt8*>(start), length)
               && memcmp(image.uuid, expected, sizeof(expected)) == 0;
    }

    struct EntryHead
    {
        mach_vm_address_t address{0};
        UInt8 bytes[8]{};
    };

    struct Image
    {
        KernelPatcher& patcher;
        size_t id;
        mach_vm_address_t start;
        size_t size;

        mach_vm_address_t resolve(const char* const symbol) const
        {
            const auto address = patcher.solveSymbol(id, symbol, start, size);
            patcher.clearError();
            return address;
        }

        bool contains(const mach_vm_address_t address, const size_t bytes) const
        {
            return address >= start && bytes <= size && address - start <= size - bytes;
        }

        template <size_t Count>
        bool matches(const mach_vm_address_t address, const UInt8 (&bytes)[Count]) const
        {
            return contains(address, Count) && memcmp(reinterpret_cast<const void*>(address), bytes, Count) == 0;
        }

        bool calls(const mach_vm_address_t address, const mach_vm_address_t target) const
        {
            if (!contains(address, 5) || !contains(target, 1)
                || *reinterpret_cast<const UInt8*>(address) != 0xE8)
            {
                return false;
            }
            SInt32 relative{};
            memcpy(&relative, reinterpret_cast<const void*>(address + 1), sizeof(relative));
            return address + 5 + static_cast<mach_vm_address_t>(static_cast<SInt64>(relative)) == target;
        }

        bool install(KernelPatcher::RouteRequest* routes, const size_t count) const
        {
            const auto result = patcher.routeMultipleLong(id, routes, count, start, size);
            patcher.clearError();
            return result;
        }

        // Lilu can report success after writing the trampoline without the jump; compare the entry before and after.
        EntryHead head(const char* const symbol) const
        {
            EntryHead entry{resolve(symbol)};
            if (entry.address != 0 && contains(entry.address, sizeof(entry.bytes))) {
                memcpy(entry.bytes, reinterpret_cast<const void*>(entry.address), sizeof(entry.bytes));
            } else {
                entry.address = 0;
            }
            return entry;
        }

        static bool patched(const EntryHead& entry)
        {
            return entry.address != 0
                   && memcmp(reinterpret_cast<const void*>(entry.address), entry.bytes, sizeof(entry.bytes)) != 0;
        }

        template <typename Function>
        bool branch(const mach_vm_address_t address, Function function) const
        {
            KernelPatcher::RouteRequest request{nullptr, function};
            request.from = address;
            return install(&request, 1);
        }
    };
}

[[gnu::used]] static mach_vm_address_t nredUnifiedPrimaryNative asm("_nredUnifiedPrimaryNative"){0};
[[gnu::used]] static mach_vm_address_t nredUnifiedPrimarySystem asm("_nredUnifiedPrimarySystem"){0};
[[gnu::used]] static mach_vm_address_t nredUnifiedBufferResume asm("_nredUnifiedBufferResume"){0};
[[gnu::used]] static mach_vm_address_t nredUnifiedPoolNative asm("_nredUnifiedPoolNative"){0};
[[gnu::used]] static mach_vm_address_t nredUnifiedPoolPageable asm("_nredUnifiedPoolPageable"){0};
[[gnu::used]] static mach_vm_address_t nredUnifiedOriginalFactory asm("_nredUnifiedOriginalFactory"){0};

extern "C" [[gnu::used, gnu::noinline]] bool nredUnifiedUseSystem(void* const resource, void* const shared)
{
    if (primaryBackingForResource(resource, shared) == BackingKind::Native) { return false; }
    if (!hasPerTaskPageTable(getGPUTask(resource))) { return false; }
    const auto* const parameters = getAllocationParameters(resource);
    return parameters != nullptr && X5000Memory::validAlignment(getMember<UInt64>(resource, allocationAlignment), PAGE_SIZE);
}

[[gnu::naked]] static void primaryBranch()
{
    asm volatile(
        "movq %r14, %rdi\n"
        "movq %r15, %rsi\n"
        "callq _nredUnifiedUseSystem\n"
        "movq 0x18(%r14), %rdi\n"
        "testb %al, %al\n"
        "movl 0xc90(%rdi), %eax\n"
        "jz 1f\n"
        "btrl $0x1a, %eax\n"
        "jmpq *_nredUnifiedPrimarySystem(%rip)\n"
        "1:\n"
        "btl $0x15, %eax\n"
        "jc 2f\n"
        "jmpq *_nredUnifiedPrimaryNative(%rip)\n"
        "2: jmpq *_nredUnifiedPrimarySystem(%rip)\n");
}

[[gnu::naked]] static void bufferBranch()
{
    asm volatile(
        "movq %rbx, %rdi\n"
        "movq %r12, %rsi\n"
        "movl %r14d, %edx\n"
        "movq %r9, %rcx\n"
        "callq _nredUnifiedMakeDataBacking\n"
        "jmpq *_nredUnifiedBufferResume(%rip)\n");
}

// Only unified-memory backings requested through allocatePooled use the native buffer pool. Every other caller keeps
// the pageable path it used before the pool existed.
[[gnu::naked]] static void poolBranch()
{
    asm volatile(
        "leaq _nredUnifiedPooledReturn(%rip), %rax\n"
        "cmpq %rax, 8(%rbp)\n"
        "jne 1f\n"
        "cmpl $0, 0x9bc(%rbx)\n"
        "je 1f\n"
        "xorl %eax, %eax\n"
        "jmpq *_nredUnifiedPoolNative(%rip)\n"
        "1: jmpq *_nredUnifiedPoolPageable(%rip)\n");
}

[[gnu::naked]] static void* allocatePooled(IOService*, void*, void*, void*, UInt32, UInt64, bool)
{
    asm volatile(
        "pushq %rbp\n"
        "movq %rsp, %rbp\n"
        "subq $16, %rsp\n"
        "movl 16(%rbp), %eax\n"
        "movl %eax, (%rsp)\n"
        "callq *_nredUnifiedOriginalFactory(%rip)\n"
        "_nredUnifiedPooledReturn:\n"
        "leaveq\n"
        "retq\n");
}

[[gnu::used]] static mach_vm_address_t nredBufferPoolCreate asm("_nredBufferPoolCreate"){0};
[[gnu::used]] static mach_vm_address_t nredBufferPoolSkip asm("_nredBufferPoolSkip"){0};

extern "C" [[gnu::used, gnu::noinline]] bool nredUnifiedBufferPool(IOService* const accelerator)
{
    if (!ownsAccelerator(accelerator)) { return false; }
    SYSLOG("UnifiedMemory", "Creating the native buffer pool for unified memory backings");
    return true;
}

// Replaces the native config test so the accelerator start path also creates the pool for the Renoir accelerator.
[[gnu::naked]] static void poolCreateBranch()
{
    asm volatile(
        "testb $0x20, 0xc92(%r14)\n"
        "jnz 1f\n"
        "movq %r14, %rdi\n"
        "callq _nredUnifiedBufferPool\n"
        "testb %al, %al\n"
        "jz 2f\n"
        "1: movl $0x1b0, %edi\n"
        "jmpq *_nredBufferPoolCreate(%rip)\n"
        "2: jmpq *_nredBufferPoolSkip(%rip)\n");
}

namespace
{
    constexpr UInt32 dataMemoryOptions = kIODirectionInOut | kIOMemoryPageable | kIOMemoryPurgeable
                                        | kIOMapCacheMask | kIOMemoryKernelUserShared;
    constexpr UInt32 pageablePrimaryOptions = kIODirectionInOut | kIOMemoryPageable | kIOMemoryPurgeable
                                             | kIOMapCopybackCache;
    static_assert(pageablePrimaryOptions == 0x363);

    bool usePageableData(void* const resource, void* const shared, const UInt32 options)
    {
        return resource != nullptr
               && getMember<UInt8>(resource, resourceType) == static_cast<UInt8>(ResourceKind::DataBuffer)
               && (getMember<UInt8>(resource, dataBufferFlags) & dataBufferNativePlacement) == 0
               && (options & ~dataMemoryOptions) == 0
               && primaryBackingForResource(resource, shared) == BackingKind::Buffer;
    }

    BackingKind backingKind(const mach_vm_address_t caller, IOService* const accelerator, void* const shared,
                            void* const resource, const UInt32 options)
    {
        if (!ownsAccelerator(accelerator) || caller != primaryFactoryReturn || options != pageablePrimaryOptions) {
            return BackingKind::Native;
        }
        return primaryBackingForResource(resource, shared);
    }

    void* ownedPool(IOService* accelerator);
    void samplePool(void* pool);

    OSObject* newPageableBacking(IOService* const accelerator, void* const task, void* const shared,
        void* const resource, const UInt32 options, const UInt64 size, const bool unshared, const BackingKind kind,
        const UInt64 alignment)
    {
        increment(pageableRequests);
        auto* const backing = static_cast<Backing*>(IOMalloc(sizeof(Backing)));
        if (backing == nullptr) {
            increment(allocationFailures);
            return nullptr;
        }
        const bool large = size > nativePoolLimit;
        const bool pooled = !large || (largePoolInstalled && __atomic_load_n(&poolLargeEnabled, __ATOMIC_RELAXED) != 0);
        if (large && pooled) { increment(poolLargeRequests); }
        auto* const memory = static_cast<OSObject*>(
            pooled ? allocatePooled(accelerator, task, shared, resource, options, size, unshared)
                   : reinterpret_cast<void* (*)(IOService*, void*, void*, void*, UInt32, UInt64, bool)>(
                         nredUnifiedOriginalFactory)(accelerator, task, shared, resource, options, size, unshared));
        if (memory == nullptr) {
            IOFree(backing, sizeof(Backing));
            increment(allocationFailures);
            return nullptr;
        }

        if (alignment != 0) {
            if (memory->metaCast(sysMemoryClass) == nullptr || !X5000Memory::validAlignment(alignment, PAGE_SIZE)) {
                memory->release();
                IOFree(backing, sizeof(Backing));
                increment(allocationFailures);
                return nullptr;
            }
            getMember<UInt32>(memory, sysMemoryAlignment) = static_cast<UInt32>(alignment);
        }

        // WindowServer recreates resources on the same backing many times per second. Keeping the orphaned mapping
        // lets the next resource reuse its GPU address and page-table entries instead of rewriting every entry. The
        // native GART collector, unwire, purge and address-space reclaim still release orphaned mappings.
        setMemoryFlags(memory, keepOrphanedMappings);

        // Pool backings arrive wired; the native wired notification ran before the backing was registered.
        const auto flags = getMember<UInt32>(memory, memoryFlags);
        const auto residency = (flags & wiredMemory) != 0 ? Residency::Wired : Residency::Cold;
        if ((flags & pooledMemory) != 0) {
            increment(pooledAllocations);
            samplePool(ownedPool(accelerator));
        }
        IOSimpleLockLock(backingLock);
        auto& bucket = bucketFor(memory);
        *backing = {memory, bucket, getMember<UInt64>(memory, memorySize), residency};
        bucket = backing;
        increment(liveBytes, backing->size);
        if (residency == Residency::Wired) { increment(residentBytes, backing->size); }
        IOSimpleLockUnlock(backingLock);
        switch (kind) {
            case BackingKind::Buffer: increment(bufferAllocations); break;
            case BackingKind::Texture: increment(textureAllocations); break;
            case BackingKind::Surface: increment(surfaceAllocations); break;
            case BackingKind::Native: break;
        }
        return memory;
    }

    [[gnu::noinline]] void* makeSystemMemory(IOService* const accelerator, void* const task, void* const shared,
        void* const resource, const UInt32 options, const UInt64 size, const bool unshared)
    {
        const auto caller = reinterpret_cast<mach_vm_address_t>(__builtin_return_address(0));
        const auto kind = backingKind(caller, accelerator, shared, resource, options);
        if (kind == BackingKind::Native) {
            return FunctionCast(makeSystemMemory, nredUnifiedOriginalFactory)(accelerator, task, shared, resource,
                                                                              options, size, unshared);
        }

        const auto alignment = resource != nullptr ? getMember<UInt64>(resource, allocationAlignment) : 0;
        return newPageableBacking(accelerator, task, shared, resource, options, size, unshared, kind, alignment);
    }

    bool allocateData(void* const resource, void* const shared, const bool preferVideo, const UInt32 options)
    {
        const auto original = FunctionCast(allocateData, originalDataAllocator);
        return original(resource, shared, preferVideo && !usePageableData(resource, shared, options), options);
    }

    extern "C" [[gnu::used, gnu::noinline]] void* nredUnifiedMakeDataBacking(void* const resource, void* const shared,
                                                                           const UInt32 options, const UInt64 size)
    {
        auto* const accelerator = getMember<IOService*>(resource, resourceAccelerator);
        if (usePageableData(resource, shared, options)) {
            return newPageableBacking(accelerator, nullptr, shared, nullptr, options | kIOMemoryPageable,
                                      size, false, BackingKind::Buffer, 0);
        }
        return FunctionCast(makeSystemMemory, nredUnifiedOriginalFactory)(accelerator, nullptr, shared, nullptr,
                                                                          options, size, false);
    }

    UInt64 getWiredBudget(IOService* const accelerator)
    {
        const auto native = getMember<UInt64>(accelerator, acceleratorWiredLimit);
        if (!ownsAccelerator(accelerator)) { return native; }
        const auto budget = X5000Memory::wiredBudget(native, requestedBudgetMiB);
        __atomic_store_n(&effectiveBudget, budget, __ATOMIC_RELAXED);
        return budget;
    }

    UInt64 getSingleAllocationBudget(IOService* const accelerator)
    {
        const auto native = FunctionCast(getSingleAllocationBudget, originalSingleLimit)(accelerator);
        if (!ownsAccelerator(accelerator) || requestedBudgetMiB == 0) { return native; }
        return X5000Memory::singleAllocationBudget(native, getWiredBudget(accelerator), PAGE_SIZE);
    }

    IOReturn getConfig(IORegistryEntry* const device, void* const config)
    {
        const auto result = FunctionCast(getConfig, originalGetConfig)(device, config);
        if (result != kIOReturnSuccess || !ownsAccelerator(getMember<IOService*>(device, deviceAccelerator))) {
            return result;
        }
        const auto* const api = OSDynamicCast(OSString, device->getProperty("API"));
        if (api == nullptr) { return result; }

        const auto wired = getMember<UInt64>(config, configWiredBudget);
        if (api->isEqualTo("GL/CL")) {
            const auto budget = X5000Memory::glCommandBudgetKiB(wired, PAGE_SIZE);
            if (budget != 0) {
                getMember<UInt32>(config, GLBudget::ConfigBudget) = budget;
                getMember<UInt32>(config, 0) |= GLBudget::ConfigCommandBudgetPresent;
                __atomic_store_n(&glBudgetKiB, budget, __ATOMIC_RELAXED);
                increment(glBudgetQueries);
            }
            return result;
        }
        if (!api->isEqualTo("Metal") || !__atomic_load_n(&metalSegmentReady, __ATOMIC_ACQUIRE)) { return result; }

        const auto budget = X5000Memory::recommendedWorkingSet(wired, PAGE_SIZE);
        if (budget != 0) {
            getMember<UInt64>(config, configTextureBudget) = budget;
            getMember<UInt32>(config, 0) |= configSeparateWorkingSet;
            __atomic_store_n(&metalWorkingSet, budget, __ATOMIC_RELAXED);
            increment(metalBudgetQueries);
        }
        return result;
    }

    void didWire(IOService* const accelerator, void* const memory)
    {
        FunctionCast(didWire, originalWired)(accelerator, memory);
        if (!ownsAccelerator(accelerator)) { return; }
        updateBackingResidency(memory, true);
        __atomic_store_n(&totalResidentBytes, getMember<UInt64>(accelerator, acceleratorWiredBytes), __ATOMIC_RELAXED);
    }

    void didUnwire(IOService* const accelerator, void* const memory)
    {
        FunctionCast(didUnwire, originalUnwired)(accelerator, memory);
        if (!ownsAccelerator(accelerator)) { return; }
        updateBackingResidency(memory, false);
        __atomic_store_n(&totalResidentBytes, getMember<UInt64>(accelerator, acceleratorWiredBytes), __ATOMIC_RELAXED);
    }

    bool requestsImmediatePurge(const UInt32 state)
    {
        // Volatile only permits discard under memory pressure; the native LRU can
        // still unwire it later. Unwiring immediately makes transient WindowServer
        // transitions rewire the same backing at 6-14 ms per surface.
        return state == kIOMemoryPurgeableEmpty;
    }

    void releasePurgeableBacking(OSObject* const memory, const UInt32 state)
    {
        if (!requestsImmediatePurge(state) || memory == nullptr || memory->metaCast(sysMemoryClass) == nullptr) {
            return;
        }
        constexpr UInt32 requiredOptions = kIOMemoryPageable | kIOMemoryPurgeable;
        const auto flags = getMember<UInt32>(memory, memoryFlags);
        if ((flags & (wiredMemory | clientBufferMemory)) != wiredMemory
            || ((flags >> purgeableShift) & purgeableMask) != state
            || (getMember<UInt32>(memory, memoryOptionsOffset) & requiredOptions) != requiredOptions
            || getPrepareCount(memory) != 0 || !allEventsComplete(memory))
        {
            return;
        }
        unwireMemory(memory);
        increment(purgeUnwires);
    }

    [[gnu::noinline]] void setResourcePurgeable(void* const resource, const UInt32 backingState,
                                               const UInt32 primaryState, UInt32* const previous)
    {
        const auto caller = reinterpret_cast<mach_vm_address_t>(__builtin_return_address(0));
        FunctionCast(setResourcePurgeable, originalSetResourcePurgeable)(resource, backingState, primaryState, previous);
        if (caller != purgeRequestReturn || !ready() || !requestsImmediatePurge(primaryState)) { return; }

        const auto kind = static_cast<ResourceKind>(getMember<UInt8>(resource, resourceType));
        if ((kind != ResourceKind::Texture && kind != ResourceKind::DataBuffer)
            || (kind == ResourceKind::Texture && getMember<void*>(resource, resourceSurface) != nullptr))
        {
            return;
        }
        auto* const primary = getMember<OSObject*>(resource, primaryMemory);
        if (!isManaged(primary)) { return; }
        auto* const backing = getMember<OSObject*>(resource, backingMemory);
        releasePurgeableBacking(primary, primaryState);
        if (backing != primary) { releasePurgeableBacking(backing, backingState); }
    }

    void freeBacking(void* const memory)
    {
        if (ready()) {
            IOSimpleLockLock(backingLock);
            auto** const link = findBacking(memory);
            auto* const backing = *link;
            if (backing != nullptr) {
                *link = backing->next;
                __atomic_sub_fetch(&liveBytes, backing->size, __ATOMIC_RELAXED);
                if (backing->residency == Residency::Wired) {
                    __atomic_sub_fetch(&residentBytes, backing->size, __ATOMIC_RELAXED);
                }
            }
            IOSimpleLockUnlock(backingLock);
            if (backing != nullptr) {
                while (backing->mappings != nullptr) {
                    auto* const mapping = backing->mappings;
                    backing->mappings = mapping->next;
                    IOFree(mapping, sizeof(TaskMapping));
                }
                IOFree(backing, sizeof(Backing));
            }
        }
        FunctionCast(freeBacking, originalFree)(memory);
    }

    template <typename Function>
    Function virtualSlot(const void* const object, const size_t slot)
    {
        return reinterpret_cast<Function>((*static_cast<void* const* const*>(object))[slot / sizeof(void*)]);
    }

    // Caller holds the accelerator lock.
    void samplePool(void* const pool)
    {
        if (pool == nullptr) { return; }
        __atomic_store_n(&activePool, pool, __ATOMIC_RELEASE);
        for (size_t bucket = 0; bucket < poolBuckets; bucket++) {
            __atomic_store_n(&poolSnapshot[bucket][0], getMember<UInt32>(pool, poolWiredCount + bucket * poolBucketStride),
                             __ATOMIC_RELAXED);
            __atomic_store_n(&poolSnapshot[bucket][1],
                             getMember<UInt32>(pool, poolPurgeableCount + bucket * poolBucketStride), __ATOMIC_RELAXED);
            __atomic_store_n(&poolSnapshot[bucket][2], getMember<UInt32>(pool, poolDirtyCount + bucket * poolDirtyStride),
                             __ATOMIC_RELAXED);
        }
    }

    void* ownedPool(IOService* const accelerator)
    {
        return accelerator != nullptr && ownsAccelerator(accelerator)
                       && getMember<UInt32>(accelerator, acceleratorPoolEnabled) != 0
                   ? getMember<void*>(accelerator, acceleratorPool)
                   : nullptr;
    }

    // Dirty pool slots are matched by the raw task pointer. IOAccelShared2::free releases its orphan pools here (under
    // the accelerator lock) before it drops its task reference, so the task's dirty slots are re-keyed to kernel_task
    // first. Kernel requests never take dirty slots, so those pieces are reused only after the pool timer zero-fills
    // them, and a later task at the same address cannot receive them.
    void releaseShared(void* const orphanPool, void* const shared)
    {
        auto* const pool = shared != nullptr && largePoolInstalled
                               ? ownedPool(getMember<IOService*>(shared, sharedAccelerator))
                               : nullptr;
        if (pool != nullptr) {
            auto* const task = getMember<void*>(shared, sharedPoolTask);
            for (size_t bucket = 0; task != nullptr && bucket < poolBuckets; bucket++) {
                auto* const slots = getMember<UInt8*>(pool, poolDirtySlots + bucket * poolDirtyStride);
                auto mask = getMember<UInt64>(pool, poolDirtyMask + bucket * poolDirtyStride);
                while (slots != nullptr && mask != 0) {
                    const auto slot = static_cast<size_t>(__builtin_ctzll(mask));
                    mask &= mask - 1;
                    auto& owner = *reinterpret_cast<void**>(slots + slot * poolDirtySlotBytes + sizeof(void*));
                    if (owner == task) {
                        owner = kernel_task;
                        increment(poolRekeyed);
                    }
                }
            }
            samplePool(pool);
        }
        FunctionCast(releaseShared, originalSharedRelease)(orphanPool, shared);
    }

    // Native orphanIt merges only the events of the mappings that are left. The PTE releases of the pruned ones are
    // added to the dirty slots that now hold this backing's descriptors. Caller holds the accelerator lock.
    void addPruneEventToSlots(OSObject* const memory)
    {
        auto* const pool = ownedPool(getMember<IOService*>(memory, memoryAccelerator));
        auto* const descriptors = getMember<void* const*>(memory, memoryDescriptors);
        const auto count = getMember<UInt32>(memory, memoryDescriptorCount);
        if (pool == nullptr || descriptors == nullptr || count == 0) { return; }
        for (size_t bucket = 0; bucket < poolBuckets; bucket++) {
            auto* const slots = getMember<UInt8*>(pool, poolDirtySlots + bucket * poolDirtyStride);
            auto* const events = getMember<UInt8*>(pool, poolDirtyEvents + bucket * poolDirtyStride);
            auto mask = getMember<UInt64>(pool, poolDirtyMask + bucket * poolDirtyStride);
            while (slots != nullptr && events != nullptr && mask != 0) {
                const auto slot = static_cast<size_t>(__builtin_ctzll(mask));
                mask &= mask - 1;
                auto* const descriptor = *reinterpret_cast<void**>(slots + slot * poolDirtySlotBytes);
                for (UInt32 index = 0; index < count; index++) {
                    if (descriptors[index] != descriptor) { continue; }
                    virtualSlot<void (*)(void*, void*, void*)>(pruneMachine, eventMergeSlot)(
                        pruneMachine, pruneEvent, events + slot * eventBytes);
                    break;
                }
            }
        }
        samplePool(pool);
    }

    // Marks a backing's descriptors as handed back (by us or by native orphanIt). Returns false if they already were.
    bool claimPoolReturn(void* const memory)
    {
        bool claimed = false;
        IOSimpleLockLock(backingLock);
        if (auto* const backing = *findBacking(memory); backing != nullptr && !backing->poolReturned) {
            backing->poolReturned = true;
            claimed = true;
        }
        IOSimpleLockUnlock(backingLock);
        return claimed;
    }

    // Holds a wired pooled backing while its orphaned mappings are pruned, so that the last release_pte does not
    // unwire every piece just before the backing is destroyed.
    bool holdsPooledWire(OSObject* const memory)
    {
        if (!largePoolInstalled || !poolReturnAvailable || !releasePTERouted
            || __atomic_load_n(&poolReturnEnabled, __ATOMIC_RELAXED) == 0)
        {
            return false;
        }
        const auto flags = getMember<UInt32>(memory, memoryFlags);
        if ((flags & (pooledMemory | wiredMemory)) != (pooledMemory | wiredMemory)) { return false; }
        virtualSlot<void (*)(OSObject*)>(memory, incrementWireSlot)(memory);
        return true;
    }

    // Same hand-back as IOAccelSysMemory::orphanIt, for a pooled backing whose orphaned mappings were all idle and
    // released. orphanIt runs after IOAccelMemory::release dropped its reference, so once the orphans are gone only
    // orphanBacking's own reference is left and the backing is destroyed when it drops it. Same-task reuse inherits the merged
    // mapping events; other tasks get the pieces after the pool timer zero-fills them once the events complete.
    bool returnPooledDescriptors(OSObject* const memory, const UInt32 dyingRetainCount)
    {
        auto* const accelerator = getMember<IOService*>(memory, memoryAccelerator);
        auto* const descriptors = getMember<void*>(memory, memoryDescriptors);
        const auto count = getMember<UInt32>(memory, memoryDescriptorCount);
        auto& flags = getMember<UInt32>(memory, memoryFlags);
        const auto state = (flags >> purgeableShift) & purgeableMask;
        auto* const pool = ownedPool(accelerator);
        if (pool == nullptr || descriptors == nullptr || count == 0 || state > volatileState
            || memory->getRetainCount() != static_cast<int>(dyingRetainCount)
            || getMember<void*>(memory, memoryResource) != nullptr || !claimPoolReturn(memory))
        {
            return false;
        }
        // pruneEvent already holds the VMPT stamps of the PTE releases; add the events of any mapping left.
        auto* const event = pruneEvent;
        mergeMappingEvents(memory, event);
        auto* const shared = getMember<void*>(memory, memoryShared);
        auto* const task = shared != nullptr ? getMember<void*>(shared, sharedPoolTask) : static_cast<void*>(kernel_task);
        // The dying backing must not re-apply a purgeable state to pieces that now belong to the pool.
        flags = (flags & ~(orphanedPurgeable | (purgeableMask << purgeableShift))) | (nonVolatileState << purgeableShift);
        releasePoolDescriptors(pool, descriptors, count, task, event);
        kickPoolTimer(accelerator);
        samplePool(pool);
        increment(poolReturns);
        increment(poolReturnPieces, count);
        return true;
    }

    // A backing is orphaned once only orphaned mappings reference it. Nothing can revive them any more, and a pooled
    // backing hands its pages back to the pool here, so idle mappings are released first, as in IOAccelMemory::release.
    // Mappings with GPU work still pending stay orphaned under the native rules. The gate is the registry rather than
    // the kept-mappings flag, because a fixed-address mapping clears the flag while earlier orphans remain.
    // Starts collecting the VMPT events of the PTE releases done by the coming prune and holds the backing wired.
    // Returns false (nothing held) unless the backing is a wired pooled one and the collector is free.
    bool beginPooledPrune(OSObject* const memory)
    {
        // The collector is used under the accelerator lock; the exchange keeps a second thread out regardless.
        thread_t expected = nullptr;
        auto* const machine = largePoolInstalled && poolReturnAvailable && releasePTERouted
                                      && __atomic_load_n(&poolReturnEnabled, __ATOMIC_RELAXED) != 0
                                  ? getEventMachine(memory)
                                  : nullptr;
        const bool collecting = machine != nullptr
                                && __atomic_compare_exchange_n(&pruneOwner, &expected, current_thread(), false,
                                                               __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
        const bool held = collecting && holdsPooledWire(memory);
        if (held) {
            pruneAccelerator = getMember<IOService*>(memory, memoryAccelerator);
            pruneMachine = machine;
            memset(pruneEvent, 0xAA, sizeof(pruneEvent));
            virtualSlot<void (*)(void*, void*)>(pruneMachine, eventInitSlot)(pruneMachine, pruneEvent);
        } else if (collecting) {
            __atomic_store_n(&pruneOwner, nullptr, __ATOMIC_RELEASE);
        }
        return held;
    }

    void endPooledPrune()
    {
        pruneMachine = nullptr;
        pruneAccelerator = nullptr;
        __atomic_store_n(&pruneOwner, nullptr, __ATOMIC_RELEASE);
    }

    void orphanBacking(OSObject* const memory)
    {
        const auto orphan = FunctionCast(orphanBacking, originalOrphan);
        if (!isManaged(memory)) {
            orphan(memory);
            return;
        }
        memory->retain();
        const bool held = beginPooledPrune(memory);
        pruneOrphanedMappings(memory);
        if (getMember<SInt16>(memory, memoryMappingCount) > 0) {
            if (held) { virtualSlot<void (*)(OSObject*)>(memory, decrementWireSlot)(memory); }
            // Native orphanIt hands a pooled backing's descriptors back itself. If they already went back, clearing the
            // pooled flag makes orphanIt skip releaseDescriptors.
            if ((getMember<UInt32>(memory, memoryFlags) & pooledMemory) != 0 && !claimPoolReturn(memory)) {
                getMember<UInt32>(memory, memoryFlags) &= ~pooledMemory;
            }
            orphan(memory);
            if (held) { addPruneEventToSlots(memory); }
        } else if (held) {
            // orphanIt runs after IOAccelMemory::release dropped its reference: only ours is left.
            if (!returnPooledDescriptors(memory, 1)) { increment(poolReturnSkips); }
            virtualSlot<void (*)(OSObject*)>(memory, decrementWireSlot)(memory);
        }
        if (held) { endPooledPrune(); }
        memory->release();
    }

    // Route of IOAccelMemory::pruneOrphanedMappings. Only the call from IOAccelMemory::release is handled: that caller
    // holds one temporary reference across the prune and drops it right after, so the backing dies when the prune
    // leaves no mapping and that reference is the only one.
    void pruneFromRelease(OSObject* const memory)
    {
        const auto original = FunctionCast(pruneFromRelease, originalPrune);
        if (reinterpret_cast<mach_vm_address_t>(__builtin_return_address(0)) != releasePruneReturn || !ready()
            || !isManaged(memory))
        {
            original(memory);
            return;
        }
        const bool held = beginPooledPrune(memory);
        original(memory);
        if (held) {
            if (getMember<SInt16>(memory, memoryMappingCount) == 0 && !returnPooledDescriptors(memory, 1)) {
                increment(poolReturnSkips);
            }
            virtualSlot<void (*)(OSObject*)>(memory, decrementWireSlot)(memory);
            endPooledPrune();
        }
    }

    // The mapping lookup returns a task's orphaned mapping without comparing the requested address. Idle orphans are
    // released before a fixed-address request so that it gets a new mapping at that address.
    OSObject* mapAtAddress(OSObject* const memory, void* const task, const UInt32 options, const UInt64 address,
                           const UInt64 length)
    {
        const auto original = FunctionCast(mapAtAddress, originalMapAt);
        if (address == 0 || length == 0 || (getMember<UInt32>(memory, memoryFlags) & keptMappings) == 0) {
            return original(memory, task, options, address, length);
        }
        memory->retain();
        pruneOrphanedMappings(memory);
        auto* const mapping = original(memory, task, options, address, length);
        memory->release();
        return mapping;
    }

    bool initMapping(void* const mapping, IOService* const accelerator, void* const task, OSObject* const memory,
                     const UInt32 options)
    {
        const auto result = FunctionCast(initMapping, originalMapInit)(mapping, accelerator, task, memory, options);
        if (!result || !ownsAccelerator(accelerator) || !hasPerTaskPageTable(task) || !isManaged(memory)) {
            return result;
        }

        auto* const entry = static_cast<TaskMapping*>(IOMalloc(sizeof(TaskMapping)));
        if (entry == nullptr) {
            increment(mappingTrackingFailures);
            return result;
        }
        bool tracked = false;
        IOSimpleLockLock(backingLock);
        if (auto* const backing = *findBacking(memory); backing != nullptr) {
            *entry = {mapping, task, backing->mappings};
            backing->mappings = entry;
            tracked = true;
        }
        IOSimpleLockUnlock(backingLock);
        if (!tracked) { IOFree(entry, sizeof(TaskMapping)); }
        return result;
    }

    void freeMapping(void* const mapping)
    {
        TaskMapping* entry = nullptr;
        if (ready()) {
            IOSimpleLockLock(backingLock);
            if (auto* const backing = *findBacking(getMember<void*>(mapping, mappingMemory))) {
                auto** const link = findMapping(*backing, mapping);
                entry = *link;
                if (entry != nullptr) { *link = entry->next; }
            }
            IOSimpleLockUnlock(backingLock);
        }
        if (entry != nullptr) { IOFree(entry, sizeof(TaskMapping)); }
        FunctionCast(freeMapping, originalMapFree)(mapping);
    }

    bool releaseMappingPTE(void* const mapping)
    {
        const auto result = FunctionCast(releaseMappingPTE, originalReleasePTE)(mapping);
        if (!result || !ready()) { return result; }
        if (__atomic_load_n(&pruneOwner, __ATOMIC_ACQUIRE) == current_thread() && pruneMachine != nullptr
            && getMember<IOService*>(mapping, mappingAccelerator) == pruneAccelerator)
        {
            auto* const task = getMember<void*>(mapping, mappingTask);
            auto* const context = task != nullptr && (getMember<UInt8>(task, taskFlags) & perTaskPageTable) != 0
                                      ? getMember<UInt8*>(task, taskPageTable)
                                      : nullptr;
            if (context != nullptr) {
                virtualSlot<void (*)(void*, void*, void*)>(pruneMachine, eventMergeSlot)(
                    pruneMachine, context + contextUpdateEvent, pruneEvent);
            }
        }

        IOSimpleLockLock(backingLock);
        if (auto* const backing = *findBacking(getMember<void*>(mapping, mappingMemory))) {
            if (auto* const entry = *findMapping(*backing, mapping); entry != nullptr && entry->committed) {
                entry->releasedAt = backing->evictionEpoch;
                entry->committed = false;
            }
        }
        IOSimpleLockUnlock(backingLock);
        return result;
    }

    bool commitMapping(void* const mapping)
    {
        const auto result = FunctionCast(commitMapping, originalCommit)(mapping);
        if (!result || !ready()) { return result; }
        auto* const task = getMember<void*>(mapping, mappingTask);
        if (!hasPerTaskPageTable(task)) { return result; }

        const auto address = getMember<UInt64>(mapping, mappingGPUAddress);
        IOSimpleLockLock(backingLock);
        if (auto* const backing = *findBacking(getMember<void*>(mapping, mappingMemory))) {
            increment(taskCommits);
            if (auto* const entry = *findMapping(*backing, mapping); entry != nullptr && address != 0) {
                if (entry->task != task || (entry->address != 0 && entry->address != address)) {
                    increment(mappingAddressChanges);
                } else if (entry->address != 0 && !entry->committed && entry->releasedAt != backing->evictionEpoch
                           && backing->residency == Residency::Wired) {
                    increment(sameAddressRecommits);
                    if (hasOtherTask(*backing, *entry)) { increment(sharedAddressRecommits); }
                }
                entry->address = address;
                entry->task = task;
                entry->committed = true;
            }
        }
        IOSimpleLockUnlock(backingLock);
        return result;
    }

    bool systemBackedDisplayResource(void* const resource)
    {
        if (resource == nullptr || !ownsAccelerator(getMember<IOService*>(resource, resourceAccelerator))) {
            return false;
        }
        auto* const owner = getMember<UInt8>(resource, resourceType) == static_cast<UInt8>(ResourceKind::SurfaceAlias)
            ? getMember<void*>(resource, resourceSurface)
            : resource;
        if (owner == nullptr) { return false; }
        auto* memory = getMember<void*>(owner, primaryMemory);
        if (memory == nullptr) { memory = getMember<void*>(owner, backingMemory); }
        return isManaged(memory);
    }

    bool getFlip(void* const display, const UInt32 index, void* const first, void* const second, const UInt32 interval,
                 void* const flip, const bool fill, const UInt32 rotation, void* const savedFlip)
    {
        if (systemBackedDisplayResource(first) || (fill && systemBackedDisplayResource(second))) {
            increment(deniedFlips);
            return false;
        }
        return FunctionCast(getFlip, originalFlip)(display, index, first, second, interval, flip, fill, rotation,
                                                  savedFlip);
    }

    // Page-table write coalescing. mapVMPT calls AMDHWVMContext::updateContiguousPTEsWithDMAUsingAddr once per
    // physical segment, and pageable memory gives one 4 KiB page per segment (mostly physically descending), so every
    // page costs one 10-dword SDMA PTE_PDE packet. A thread entering AMDHWVMContext::mapVA claims the coalescing scope;
    // inside it, runs of single-entry updates for consecutive page-table entries are collected and written as one SDMA
    // WRITE_LINEAR packet with explicit entries (2 dwords each), as amdgpu's sdma_v4_0_vm_write_pte does. The run is
    // written before any other reservation or submission the thread makes on the pool and whenever the outermost
    // mapVMPT returns, i.e. before mapVA ends the update (endVMPTUpdate skips an empty pool). mapVA calls were never
    // seen overlapping (828 calls, at most one active). SDMA stays the only page-table writer.
    constexpr UInt32 sdmaWriteLinear = 2;  // SDMA_OP_WRITE, SDMA_SUBOP_WRITE_LINEAR = 0
    constexpr UInt32 pteBatchMax = 128;

    struct PTEBatch
    {
        void* context{nullptr};
        void* pool{nullptr};
        UInt64 firstEntry{0};
        UInt64 flags{0};
        UInt32 count{0};
        UInt64 addresses[pteBatchMax]{};
    };

    UInt32 pteCoalesce{1};
    thread_t pteOwner{nullptr};  // thread inside the outermost coalescing mapVMPT call
    UInt32 pteDepth{0};          // mapVMPT nesting of pteOwner; only pteOwner reads or writes it and pteBatch
    PTEBatch pteBatch{};
    mach_vm_address_t originalPTEUpdate{0};
    mach_vm_address_t originalMapVMPT{0};
    mach_vm_address_t originalMapVA{0};
    mach_vm_address_t originalPoolReserve{0};
    mach_vm_address_t originalPoolSubmit{0};
    void (*poolCommit)(void*, UInt32*){nullptr};
    UInt64 pteBatches{0};
    UInt64 pteBatchedEntries{0};
    UInt64 pteDirectCalls{0};
    UInt64 pteFallbacks{0};

    bool ownsPTEBatch() { return __atomic_load_n(&pteOwner, __ATOMIC_ACQUIRE) == current_thread(); }

    void writePTEsNative(void* const context, const UInt64 entry, const UInt64 count, const UInt64 addr,
                         const UInt64 flags, const UInt64 step)
    {
        using Update = void (*)(void*, UInt64, UInt64, UInt64, UInt64, UInt64);
        reinterpret_cast<Update>(originalPTEUpdate)(context, entry, count, addr, flags, step);
    }

    // Writes the owner's pending run into its pool. If the pool cannot take the packet, the entries go through the
    // native writer one by one, so nothing is dropped.
    void flushPTEBatch()
    {
        if (pteBatch.count == 0) { return; }
        const auto count = pteBatch.count;
        pteBatch.count = 0;
        using Reserve = UInt32* (*)(void*, UInt32);
        const UInt32 dwords = 4 + 2 * count;
        auto* const out = reinterpret_cast<Reserve>(originalPoolReserve)(pteBatch.pool, dwords);
        if (out == nullptr) {
            increment(pteFallbacks);
            for (UInt32 i = 0; i < count; i++) {
                writePTEsNative(pteBatch.context, pteBatch.firstEntry + 8ULL * i, 1, pteBatch.addresses[i], pteBatch.flags,
                                PAGE_SIZE);
            }
            return;
        }
        out[0] = sdmaWriteLinear;
        out[1] = static_cast<UInt32>(pteBatch.firstEntry);
        out[2] = static_cast<UInt32>(pteBatch.firstEntry >> 32);
        out[3] = 2 * count - 1;
        for (UInt32 i = 0; i < count; i++) {
            const auto value = pteBatch.addresses[i] | pteBatch.flags;
            out[4 + 2 * i] = static_cast<UInt32>(value);
            out[5 + 2 * i] = static_cast<UInt32>(value >> 32);
        }
        poolCommit(pteBatch.pool, out + dwords);
        increment(pteBatches);
        increment(pteBatchedEntries, count);
    }

    // AMDHWVMContext::updateContiguousPTEsWithDMAUsingAddr(entry, count, addr, flags, increment). Only updates made
    // inside mapVMPT by the scope owner are collected.
    void updatePTEs(void* const context, const UInt64 entry, const UInt64 count, const UInt64 addr, const UInt64 flags,
                    const UInt64 step)
    {
        if (!ownsPTEBatch() || pteDepth == 0) {
            writePTEsNative(context, entry, count, addr, flags, step);
            return;
        }
        auto* const pool = getMember<void*>(context, 0x30);
        if (count == 1 && (entry & 7) == 0) {
            const bool extends = pteBatch.count != 0 && pteBatch.context == context && pteBatch.pool == pool
                                 && pteBatch.flags == flags && pteBatch.count < pteBatchMax
                                 && entry == pteBatch.firstEntry + 8ULL * pteBatch.count;
            if (!extends) {
                flushPTEBatch();
                pteBatch.context = context;
                pteBatch.pool = pool;
                pteBatch.firstEntry = entry;
                pteBatch.flags = flags;
            }
            pteBatch.addresses[pteBatch.count++] = addr;
            return;
        }
        flushPTEBatch();
        increment(pteDirectCalls);
        writePTEsNative(context, entry, count, addr, flags, step);
    }

    // AMDHWVMContext::mapVA(va, memory, offset, length, flags): claims the coalescing scope for this thread.
    UInt64 mapVA(void* const context, const UInt64 va, void* const memory, const UInt64 offset, const UInt64 length,
                 const UInt32 flags)
    {
        const auto original = FunctionCast(mapVA, originalMapVA);
        thread_t expected = nullptr;
        const bool owner = __atomic_load_n(&pteCoalesce, __ATOMIC_RELAXED) != 0
                           && __atomic_compare_exchange_n(&pteOwner, &expected, current_thread(), false,
                                                          __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        if (owner) { pteDepth = 0; }
        const auto result = original(context, va, memory, offset, length, flags);
        if (owner) {
            flushPTEBatch();  // normally already empty: the outermost mapVMPT return flushed it
            __atomic_store_n(&pteOwner, static_cast<thread_t>(nullptr), __ATOMIC_RELEASE);
        }
        return result;
    }

    // AMDHWVMContext::mapVMPT (recursive): the run is flushed whenever the outermost call returns.
    UInt64 mapVMPT(void* const context, void* const control, const UInt32 level, const UInt32 index, const UInt64 a,
                   const UInt64 b, const UInt64 c)
    {
        const auto original = FunctionCast(mapVMPT, originalMapVMPT);
        const bool owner = ownsPTEBatch();
        if (owner) { pteDepth++; }
        const auto result = original(context, control, level, index, a, b, c);
        if (owner && --pteDepth == 0) { flushPTEBatch(); }
        return result;
    }

    // AMDCommandBufferPool::getBufferStartPtr: anything else this thread writes to the pool goes after the run.
    // Only the owner thread touches the run, so the owner check comes first.
    UInt32* reservePoolHook(void* const pool, const UInt32 dwords)
    {
        if (ownsPTEBatch() && pteBatch.count != 0 && pteBatch.pool == pool) { flushPTEBatch(); }
        using Reserve = UInt32* (*)(void*, UInt32);
        return reinterpret_cast<Reserve>(originalPoolReserve)(pool, dwords);
    }

    // AMDCommandBufferPool::submitBuffer: a run of this pool belongs to the buffer being submitted.
    void submitPoolHook(void* const pool, const UInt64 stamp)
    {
        if (ownsPTEBatch() && pteBatch.count != 0 && pteBatch.pool == pool) { flushPTEBatch(); }
        FunctionCast(submitPoolHook, originalPoolSubmit)(pool, stamp);
    }

    SYSCTL_NODE(_debug, OID_AUTO, nred_uma, CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, "X5000 unified memory");
    SYSCTL_OID(_debug_nred_uma, OID_AUTO, active, CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED, &active, 0,
               sysctl_handle_int, "IU", "Unified memory routes installed");

#define UMA_COUNTER(name, value, description)                                                             \
    SYSCTL_OID(_debug_nred_uma, OID_AUTO, name, CTLTYPE_QUAD | CTLFLAG_RD | CTLFLAG_LOCKED, &value, 0,      \
               sysctl_handle_quad, "QU", description)
    UMA_COUNTER(pageable_requests, pageableRequests, "Pageable resource backing requests");
    UMA_COUNTER(pool_allocations, pooledAllocations, "Backings served pre-wired by the native buffer pool");
    UMA_COUNTER(allocation_failures, allocationFailures, "Failed pageable backing allocations");
    UMA_COUNTER(data_buffers, bufferAllocations, "Data buffers allocated with pageable backing");
    UMA_COUNTER(textures, textureAllocations, "Textures allocated with pageable primary backing");
    UMA_COUNTER(surfaces, surfaceAllocations, "Surfaces allocated with pageable primary backing");
    UMA_COUNTER(bytes_live, liveBytes, "Logical size of managed backing objects");
    UMA_COUNTER(wire_count, wireCount, "Managed backing wire events");
    UMA_COUNTER(unwire_count, unwireCount, "Managed backing unwired before destruction");
    UMA_COUNTER(rewire_count, rewireCount, "Managed backing wired again after eviction");
    UMA_COUNTER(purge_unwires, purgeUnwires, "Idle backings unwired for explicit purgeability requests");
    UMA_COUNTER(same_va_recommits, sameAddressRecommits, "Primary mappings restored to their task and VA after eviction");
    UMA_COUNTER(shared_va_recommits, sharedAddressRecommits, "Restored mappings for primary backing shared by GPU tasks");
    UMA_COUNTER(mapping_va_changes, mappingAddressChanges, "Primary mappings observed changing GPU task or VA");
    UMA_COUNTER(mapping_tracking_failures, mappingTrackingFailures, "Failed primary mapping records");
    UMA_COUNTER(task_commits, taskCommits, "Managed backing committed into per-task GPU page tables");
    UMA_COUNTER(bytes_unwired, unwiredBytes, "Cumulative managed bytes unwired before destruction");
    UMA_COUNTER(bytes_resident, residentBytes, "Logical size of wired managed backing objects");
    UMA_COUNTER(bytes_total_wired, totalResidentBytes, "Total system bytes wired by the accelerator");
    UMA_COUNTER(scanout_denials, deniedFlips, "System memory resources declined for native scanout");
    UMA_COUNTER(wired_budget, effectiveBudget, "Effective accelerator wired system memory limit");
    UMA_COUNTER(metal_working_set, metalWorkingSet, "Recommended working set reported to Metal");
    UMA_COUNTER(metal_budget_pages, metalBudgetPages, "Validated Metal budget code pages updated");
    UMA_COUNTER(metal_budget_queries, metalBudgetQueries, "Metal device configurations with a system memory budget");
    UMA_COUNTER(gl_command_budget_kib, glBudgetKiB, "OpenGL command resource budget in KiB");
    UMA_COUNTER(gl_budget_sites, glBudgetSites, "OpenGL budget adapter sites observed");
    UMA_COUNTER(gl_budget_queries, glBudgetQueries, "OpenGL device configurations with a system memory budget");
    UMA_COUNTER(pool_returns, poolReturns, "Destroyed pooled backings whose descriptors went back to the pool");
    UMA_COUNTER(pool_return_pieces, poolReturnPieces, "Descriptors handed back to the pool on destruction");
    UMA_COUNTER(pool_return_skips, poolReturnSkips, "Destroyed wired pooled backings not handed back");
    UMA_COUNTER(pool_large_requests, poolLargeRequests, "Backings above 24 MiB requested from the pool");
    UMA_COUNTER(pool_rekeyed, poolRekeyed, "Dirty pool slots handed to kernel_task when their task released a shared");
    UMA_COUNTER(pte_batches, pteBatches, "SDMA WRITE_LINEAR packets written for coalesced page-table entries");
    UMA_COUNTER(pte_batched_entries, pteBatchedEntries, "Page-table entries written through coalesced packets");
    UMA_COUNTER(pte_direct_calls, pteDirectCalls, "Page-table updates passed to the native packet writer while coalescing");
    UMA_COUNTER(pte_fallbacks, pteFallbacks, "Coalesced runs written entry by entry because the pool was full");
#undef UMA_COUNTER

    int handlePoolSwitch(struct sysctl_oid* oidp, void* arg1, int, struct sysctl_req* req)
    {
        auto* const target = static_cast<UInt32*>(arg1);
        // Reads report the effective state: on only when the feature's routes were verified.
        const bool available = largePoolInstalled && (target != &poolReturnEnabled || (poolReturnAvailable && releasePTERouted));
        int value = available && __atomic_load_n(target, __ATOMIC_RELAXED) != 0 ? 1 : 0;
        const auto error = sysctl_handle_int(oidp, &value, 0, req);
        if (error != 0 || req->newptr == USER_ADDR_NULL) { return error; }
        if (value != 0 && value != 1) { return EINVAL; }
        if (value == 1
            && ((target == &poolReturnEnabled && (!poolReturnAvailable || !releasePTERouted)) || !largePoolInstalled))
        {
            return ENOTSUP;
        }
        __atomic_store_n(target, static_cast<UInt32>(value), __ATOMIC_RELAXED);
        return 0;
    }
    SYSCTL_PROC(_debug_nred_uma, OID_AUTO, pool_return, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, &poolReturnEnabled,
                0, handlePoolSwitch, "I", "Hand pooled descriptors back when a backing is destroyed");
    SYSCTL_PROC(_debug_nred_uma, OID_AUTO, pool_large, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, &poolLargeEnabled,
                0, handlePoolSwitch, "I", "Serve backings above 24 MiB from the pool");

    int handlePTECoalesce(struct sysctl_oid* const oidp, void*, int, struct sysctl_req* const req)
    {
        int value = poolCommit != nullptr && __atomic_load_n(&pteCoalesce, __ATOMIC_RELAXED) != 0 ? 1 : 0;
        const auto error = sysctl_handle_int(oidp, &value, 0, req);
        if (error != 0 || req->newptr == USER_ADDR_NULL) { return error; }
        if (value != 0 && value != 1) { return EINVAL; }
        if (originalPTEUpdate == 0 || originalMapVMPT == 0 || originalMapVA == 0 || originalPoolReserve == 0
            || originalPoolSubmit == 0 || poolCommit == nullptr)
        {
            return ENODEV;
        }
        __atomic_store_n(&pteCoalesce, static_cast<UInt32>(value), __ATOMIC_RELAXED);
        return 0;
    }
    SYSCTL_PROC(_debug_nred_uma, OID_AUTO, pte_coalesce, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0,
                handlePTECoalesce, "I", "Write runs of single-page PTE updates as one SDMA WRITE_LINEAR packet");


    // Bucket occupancy of the native pool (wired clean, purgeable clean, dirty per bucket), as last sampled under the
    // accelerator lock by a pooled allocation, a hand-back or a shared release.
    int handlePoolState(struct sysctl_oid* oidp, void*, int, struct sysctl_req* req)
    {
        char text[160] = "none";
        if (__atomic_load_n(&activePool, __ATOMIC_ACQUIRE) != nullptr) {
            size_t used = 0;
            for (size_t bucket = 0; bucket < poolBuckets && used < sizeof(text); bucket++) {
                const auto written = snprintf(text + used, sizeof(text) - used, "%s%u/%u/%u", bucket ? " " : "",
                    __atomic_load_n(&poolSnapshot[bucket][0], __ATOMIC_RELAXED),
                    __atomic_load_n(&poolSnapshot[bucket][1], __ATOMIC_RELAXED),
                    __atomic_load_n(&poolSnapshot[bucket][2], __ATOMIC_RELAXED));
                if (written < 0) { break; }
                used += static_cast<size_t>(written);
            }
        }
        return sysctl_handle_string(oidp, text, sizeof(text), req);
    }
    SYSCTL_PROC(_debug_nred_uma, OID_AUTO, pool_state, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0,
                handlePoolState, "A", "Pool wired/purgeable/dirty descriptors per bucket");

    void activate()
    {
        if (!ioReady || !amdReady || ready()) { return; }
        struct sysctl_oid* const controls[] = {
            &sysctl__debug_nred_uma, &sysctl__debug_nred_uma_active, &sysctl__debug_nred_uma_pageable_requests,
            &sysctl__debug_nred_uma_pool_allocations, &sysctl__debug_nred_uma_allocation_failures,
            &sysctl__debug_nred_uma_data_buffers, &sysctl__debug_nred_uma_textures, &sysctl__debug_nred_uma_surfaces,
            &sysctl__debug_nred_uma_bytes_live, &sysctl__debug_nred_uma_wire_count, &sysctl__debug_nred_uma_unwire_count,
            &sysctl__debug_nred_uma_rewire_count, &sysctl__debug_nred_uma_purge_unwires,
            &sysctl__debug_nred_uma_same_va_recommits,
            &sysctl__debug_nred_uma_shared_va_recommits, &sysctl__debug_nred_uma_mapping_va_changes,
            &sysctl__debug_nred_uma_mapping_tracking_failures,
            &sysctl__debug_nred_uma_task_commits, &sysctl__debug_nred_uma_bytes_unwired,
            &sysctl__debug_nred_uma_bytes_resident, &sysctl__debug_nred_uma_bytes_total_wired,
            &sysctl__debug_nred_uma_scanout_denials, &sysctl__debug_nred_uma_wired_budget,
            &sysctl__debug_nred_uma_metal_working_set, &sysctl__debug_nred_uma_metal_budget_pages,
            &sysctl__debug_nred_uma_metal_budget_queries,
            &sysctl__debug_nred_uma_gl_command_budget_kib, &sysctl__debug_nred_uma_gl_budget_sites,
            &sysctl__debug_nred_uma_gl_budget_queries, &sysctl__debug_nred_uma_pool_returns,
            &sysctl__debug_nred_uma_pool_return_pieces, &sysctl__debug_nred_uma_pool_return_skips,
            &sysctl__debug_nred_uma_pool_large_requests, &sysctl__debug_nred_uma_pool_return,
            &sysctl__debug_nred_uma_pool_large, &sysctl__debug_nred_uma_pool_state, &sysctl__debug_nred_uma_pool_rekeyed,
            &sysctl__debug_nred_uma_pte_coalesce, &sysctl__debug_nred_uma_pte_batches,
            &sysctl__debug_nred_uma_pte_batched_entries, &sysctl__debug_nred_uma_pte_direct_calls,
            &sysctl__debug_nred_uma_pte_fallbacks,
        };
        for (auto* const control : controls) { sysctl_register_oid(control); }
        __atomic_store_n(&active, 1, __ATOMIC_RELEASE);
        SYSLOG("UnifiedMemory", "Installed X5000 pageable backing, residency budget, and display routes");
    }

    UInt32* poolWiredTable{nullptr};
    UInt32* poolPurgeableTable{nullptr};

    // Puts the native tables back if a later check fails before the pool exists.
    void restoreNativePoolBuckets()
    {
        if (poolWiredTable == nullptr || poolPurgeableTable == nullptr) { return; }
        if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS) {
            SYSLOG("UnifiedMemory", "Could not restore the native buffer pool buckets");
            return;
        }
        memcpy(poolWiredTable, nativeWiredInfo, sizeof(nativeWiredInfo));
        memcpy(poolPurgeableTable, nativePurgeableInfo, sizeof(nativePurgeableInfo));
        MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
        SYSLOG("UnifiedMemory", "Native buffer pool buckets restored");
    }

    // Rewrites the pool bucket tables before IOGraphicsAccelerator2::start creates the pool. Returns false without
    // writing when any byte differs from 25G332.
    bool installLargePoolBuckets(const Image& image)
    {
        const auto init = image.resolve(poolInitSymbol);
        const auto request = image.resolve(poolFactorySymbol);
        if (init == 0 || request == 0 || !image.matches(init + poolInfoLoad, poolInfoLea)
            || !image.matches(init + poolPurgeableLoad, poolInfoLea) || !image.matches(init + poolPrefillStore, poolPrefill)
            || !image.matches(request + poolCapCheck, poolCap) || !image.matches(request + poolStackClear, poolStack))
        {
            return false;
        }
        const auto target = [&](const size_t load) {
            SInt32 relative{};
            memcpy(&relative, reinterpret_cast<const void*>(init + load + 3), sizeof(relative));
            return init + load + 7 + static_cast<mach_vm_address_t>(static_cast<SInt64>(relative));
        };
        auto* const wired = reinterpret_cast<UInt32*>(target(poolInfoLoad));
        auto* const purgeable = reinterpret_cast<UInt32*>(target(poolPurgeableLoad));
        if (!image.contains(reinterpret_cast<mach_vm_address_t>(wired), sizeof(nativeWiredInfo))
            || !image.contains(reinterpret_cast<mach_vm_address_t>(purgeable), sizeof(nativePurgeableInfo))
            || memcmp(wired, nativeWiredInfo, sizeof(nativeWiredInfo)) != 0
            || memcmp(purgeable, nativePurgeableInfo, sizeof(nativePurgeableInfo)) != 0
            || MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS)
        {
            return false;
        }
        memcpy(wired, largeWiredInfo, sizeof(largeWiredInfo));
        memcpy(purgeable, largePurgeableInfo, sizeof(largePurgeableInfo));
        const bool written = memcmp(wired, largeWiredInfo, sizeof(largeWiredInfo)) == 0
                             && memcmp(purgeable, largePurgeableInfo, sizeof(largePurgeableInfo)) == 0;
        if (!written) {
            memcpy(wired, nativeWiredInfo, sizeof(nativeWiredInfo));
            memcpy(purgeable, nativePurgeableInfo, sizeof(nativePurgeableInfo));
        }
        MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
        if (written) {
            poolWiredTable = wired;
            poolPurgeableTable = purgeable;
        }
        return written;
    }

    bool installIOAccel(const Image& image)
    {
        const auto primary = image.resolve(allocPrimarySymbol);
        const auto buffer = image.resolve(allocDataSymbol);
        const auto clientBuffer = image.resolve(clientBufferSymbol);
        const auto factory = image.resolve(sysFactorySymbol);
        const auto pool = image.resolve(poolFactorySymbol);
        const auto start = image.resolve(acceleratorStartSymbol);
        const auto setFlags = image.resolve(setFlagsSymbol);
        const auto mapRelease = image.resolve(mapReleaseSymbol);
        const auto prune = image.resolve(pruneSymbol);
        const auto poolNew = image.resolve(poolNewSymbol);
        const auto limit = image.resolve(wiredLimitSymbol);
        const auto singleLimit = image.resolve(singleLimitSymbol);
        const auto task = image.resolve(taskSymbol);
        const auto storage = image.resolve(storageSymbol);
        const auto sharedInit = image.resolve(sharedInitSymbol);
        const auto config = image.resolve(configSymbol);
        const auto purgeRequest = image.resolve(purgeRequestSymbol);
        const auto resourcePurgeable = image.resolve(resourcePurgeableSymbol);
        const auto memoryPurgeable = image.resolve(memoryPurgeableSymbol);
        const auto prepareCount = image.resolve(prepareCountSymbol);
        const auto testEvents = image.resolve(testEventsSymbol);
        const auto unwire = image.resolve(unwireSymbol);
        if (!image.matches(primary + primaryGateOffset, primaryGate)
            || !image.matches(primary + primaryNativeSystem, nativeSystemHead)
            || !image.matches(primary + primaryOptionsOffset, primaryOptionsHead)
            || !image.calls(primary + primaryFactoryCall, factory)
            || !image.matches(primary + primaryFactoryCall + 5, primaryPostFactory)
            || !image.matches(buffer + bufferFactoryArgsOffset, bufferFactoryArgs)
            || !image.calls(buffer + bufferFactoryCall, factory)
            || !image.matches(buffer + bufferFactoryCall + 5, bufferPostFactory)
            || !image.matches(buffer + bufferCPUMappingOffset, bufferCPUMappingGate)
            || !image.matches(purgeRequest + purgeRequestCall - sizeof(purgeRequestArgs), purgeRequestArgs)
            || !image.calls(purgeRequest + purgeRequestCall, resourcePurgeable)
            || !image.matches(resourcePurgeable + 0x92, purgeablePrimaryLoad)
            || !image.matches(memoryPurgeable + 0x1C, purgeableFlagsLoad)
            || !image.matches(factory + 0x33B, memoryOptionsStore)
            || !image.matches(prepareCount, prepareCountHead) || !image.matches(testEvents, testEventsHead)
            || !image.matches(unwire + 0x189, unwireClearsWired)
            || !image.matches(clientBuffer + 0x19D, clientBufferFlagStore)
            || !image.matches(factory + poolGateOffset, poolGate)
            || !image.matches(factory + pageableBodyOffset, pageableHead)
            || !image.calls(factory + poolFactoryCall, pool) || !image.matches(start + poolCreateGateOffset, poolCreateGate)
            || !image.calls(start + poolCreateCall, poolNew) || !image.matches(setFlags + 0x0E, setFlagsKeepMappings)
            || !image.matches(mapRelease + 0x24, mapReleaseKeepTest) || !image.matches(prune + 0x1A, pruneMappingCount)
            || !image.matches(limit, wiredLimitBody)
            || !image.matches(singleLimit, singleLimitHead) || !image.matches(task, taskHead)
            || !image.matches(storage + 0x86, surfaceOwnerLoad)
            || !image.matches(sharedInit + 0x37, sharedTaskStore)
            || !image.matches(config + 0x11, configAcceleratorLoad) || !image.matches(config + 0x6B, configBitsStore)
            || !image.matches(config + 0x91, configTextureStore) || !image.matches(config + 0xAE, configWiredStore))
        {
            return false;
        }

        primaryFactoryReturn = primary + primaryFactoryCall + 5;
        purgeRequestReturn = purgeRequest + purgeRequestCall + 5;
        getGPUTask = FunctionCast(getGPUTask, task);
        getPrepareCount = FunctionCast(getPrepareCount, prepareCount);
        allEventsComplete = FunctionCast(allEventsComplete, testEvents);
        unwireMemory = FunctionCast(unwireMemory, unwire);
        setMemoryFlags = FunctionCast(setMemoryFlags, setFlags);
        pruneOrphanedMappings = FunctionCast(pruneOrphanedMappings, prune);
        const auto poolRelease = image.resolve(poolReleaseSymbol);
        const auto poolKick = image.resolve(poolKickSymbol);
        const auto eventMachine = image.resolve(eventMachineSymbol);
        const auto mergeEvents = image.resolve(mergeEventsSymbol);
        if (poolRelease != 0 && poolKick != 0 && eventMachine != 0 && mergeEvents != 0) {
            releasePoolDescriptors = FunctionCast(releasePoolDescriptors, poolRelease);
            kickPoolTimer = FunctionCast(kickPoolTimer, poolKick);
            getEventMachine = FunctionCast(getEventMachine, eventMachine);
            mergeMappingEvents = FunctionCast(mergeMappingEvents, mergeEvents);
            largePoolInstalled = installLargePoolBuckets(image);
        }
        SYSLOG("UnifiedMemory", "Large buffer pool buckets %s", largePoolInstalled ? "installed" : "not installed");
        nredUnifiedPrimaryNative = primary + primaryGateOffset + sizeof(primaryGate);
        nredUnifiedPrimarySystem = primary + primaryNativeSystem;
        nredUnifiedBufferResume = buffer + bufferFactoryCall + 5;
        nredUnifiedPoolNative = factory + poolGateOffset + sizeof(poolGate);
        nredUnifiedPoolPageable = factory + pageableBodyOffset;
        nredBufferPoolCreate = start + poolCreateCall;
        nredBufferPoolSkip = start + poolCreateSkip;

        KernelPatcher::RouteRequest routes[] = {
            {allocDataSymbol, allocateData, originalDataAllocator},
            {sysFactorySymbol, makeSystemMemory, nredUnifiedOriginalFactory},
            {singleLimitSymbol, getSingleAllocationBudget, originalSingleLimit},
            {configSymbol, getConfig, originalGetConfig},
            {wiredSymbol, didWire, originalWired},
            {unwiredSymbol, didUnwire, originalUnwired},
            {resourcePurgeableSymbol, setResourcePurgeable, originalSetResourcePurgeable},
            {freeSymbol, freeBacking, originalFree},
            {orphanSymbol, orphanBacking, originalOrphan},
            {mapAtSymbol, mapAtAddress, originalMapAt},
        };
        const auto memoryRelease = image.resolve(memoryReleaseSymbol);
        const auto sharedReleaseEntry = image.resolve(sharedReleaseSymbol);
        UInt8 sharedHead[8]{}, pruneHead[8]{};
        if (sharedReleaseEntry != 0 && image.contains(sharedReleaseEntry, sizeof(sharedHead))
            && image.contains(prune, sizeof(pruneHead)))
        {
            memcpy(sharedHead, reinterpret_cast<const void*>(sharedReleaseEntry), sizeof(sharedHead));
            memcpy(pruneHead, reinterpret_cast<const void*>(prune), sizeof(pruneHead));
        } else if (largePoolInstalled) {
            largePoolInstalled = false;
            restoreNativePoolBuckets();
        }
        if (largePoolInstalled
            && (memoryRelease == 0 || !image.calls(memoryRelease + releasePruneCall, prune)
                || !image.matches(memoryRelease + releasePruneCall + 5, releasePruneTail)))
        {
            largePoolInstalled = false;
            restoreNativePoolBuckets();
            SYSLOG("UnifiedMemory", "IOAccelMemory::release does not match; pooled hand-back disabled");
        }
        if (largePoolInstalled) {
            releasePruneReturn = memoryRelease + releasePruneCall + 5;
            KernelPatcher::RouteRequest shared[] = {
                {sharedReleaseSymbol, releaseShared, originalSharedRelease},
                {pruneSymbol, pruneFromRelease, originalPrune},
            };
            const bool installed = image.install(shared, arrsize(shared)) && originalSharedRelease != 0
                                   && originalPrune != 0;
            // Lilu can report success after writing the trampoline without the jump; require both entries patched.
            const bool patched = memcmp(reinterpret_cast<const void*>(sharedReleaseEntry), sharedHead, sizeof(sharedHead)) != 0
                                 && memcmp(reinterpret_cast<const void*>(prune), pruneHead, sizeof(pruneHead)) != 0;
            poolReturnAvailable = installed && patched;
            if (!poolReturnAvailable) {
                // Without the re-keying, dirty slots must not be produced at all.
                __atomic_store_n(&poolReturnEnabled, 0, __ATOMIC_RELAXED);
                largePoolInstalled = false;
                restoreNativePoolBuckets();
                SYSLOG("UnifiedMemory", "Shared release or prune route failed; pooled hand-back disabled");
                poolReturnAvailable = false;
            }
            // NootedRed's own prune calls (orphanBacking, mapAtAddress) bypass the route.
            if (originalPrune != 0) { pruneOrphanedMappings = FunctionCast(pruneOrphanedMappings, originalPrune); }
        }
        return image.install(routes, arrsize(routes)) && image.branch(limit, getWiredBudget)
               && image.branch(factory + poolGateOffset, poolBranch)
               && image.branch(start + poolCreateGateOffset, poolCreateBranch)
               && image.branch(buffer + bufferFactoryArgsOffset, bufferBranch)
               && image.branch(primary + primaryGateOffset, primaryBranch);
    }

    bool installX5000(const Image& image)
    {
        const auto accelerator = image.resolve("__ZN43AMDRadeonX5000_AMDVega10GraphicsAccelerator10gMetaClassE");
        const auto sysMemory = image.resolve("__ZN32AMDRadeonX5000_AMDAccelSysMemory10gMetaClassE");
        const auto parameters = image.resolve(allocationParametersSymbol);
        const auto sysInit = image.resolve(sysInitSymbol);
        const auto mapInit = image.resolve(mapInitSymbol);
        const auto mapFree = image.resolve(mapFreeSymbol);
        const auto flip = image.resolve(flipSymbol);
        const auto commit = image.resolve(commitSymbol);
        const auto releasePTE = image.resolve(releasePTESymbol);
        const auto layout = image.resolve(surfaceLayoutSymbol);
        if (!image.contains(accelerator, sizeof(void*)) || !image.contains(sysMemory, sizeof(void*))
            || !image.matches(parameters + 0x1A, nativeAlignmentField) || !image.matches(sysInit + 0x12, sysAlignmentInit)
            || !image.matches(mapInit + 0xC8, sysAlignmentRead) || !image.matches(flip + 0x78, flipEligibility)
            || !image.matches(mapFree, mapFreeHead)
            || !image.matches(commit + 0x11, mappingTaskTest) || !image.matches(commit + 0x30, commitPageTableLoad)
            || !image.matches(releasePTE + 0xD, mappingTaskTest)
            || !image.matches(releasePTE + 0x1D, releasePageTableLoad)
            || !image.matches(layout + 0x1C, surfaceLayoutGate))
        {
            return false;
        }
        acceleratorClass = reinterpret_cast<const OSMetaClass*>(accelerator);
        sysMemoryClass = reinterpret_cast<const OSMetaClass*>(sysMemory);
        getAllocationParameters = FunctionCast(getAllocationParameters, parameters);
        const auto releaseHead = image.head(releasePTESymbol);
        KernelPatcher::RouteRequest routes[] = {
            {flipSymbol, getFlip, originalFlip},
            {mapInitSymbol, initMapping, originalMapInit},
            {mapFreeSymbol, freeMapping, originalMapFree},
            {commitSymbol, commitMapping, originalCommit},
            {releasePTESymbol, releaseMappingPTE, originalReleasePTE},
        };
        if (!image.install(routes, arrsize(routes))) { return false; }
        releasePTERouted = Image::patched(releaseHead) && originalReleasePTE != 0;

        // Optional: page-table write coalescing. A failure leaves it unavailable and changes nothing else.
        const auto commitFn = image.resolve(poolCommitSymbol);
        const EntryHead pteHeads[] = {image.head(poolReserveSymbol), image.head(poolSubmitSymbol),
                                      image.head(pteUpdateSymbol), image.head(mapVMPTSymbol), image.head(mapVASymbol)};
        bool pteReady = false;
        if (commitFn != 0) {
            poolCommit = reinterpret_cast<void (*)(void*, UInt32*)>(commitFn);
            KernelPatcher::RouteRequest pteRoutes[] = {
                {poolReserveSymbol, reservePoolHook, originalPoolReserve},
                {poolSubmitSymbol, submitPoolHook, originalPoolSubmit},
                {pteUpdateSymbol, updatePTEs, originalPTEUpdate},
                {mapVMPTSymbol, mapVMPT, originalMapVMPT},
                {mapVASymbol, mapVA, originalMapVA},
            };
            pteReady = image.install(pteRoutes, arrsize(pteRoutes)) && originalPoolReserve != 0 && originalPoolSubmit != 0
                       && originalPTEUpdate != 0 && originalMapVMPT != 0 && originalMapVA != 0;
            for (const auto& entry : pteHeads) { pteReady = pteReady && Image::patched(entry); }
        }
        if (!pteReady) {
            // Routes that did get installed pass everything through while coalescing is off.
            poolCommit = nullptr;
            __atomic_store_n(&pteCoalesce, 0, __ATOMIC_RELAXED);
            SYSLOG("UnifiedMemory", "Page-table write coalescing unavailable");
        }
        if (!releasePTERouted && largePoolInstalled) {
            // X5000 loads before its accelerator starts, so the pool does not exist yet.
            largePoolInstalled = false;
            restoreNativePoolBuckets();
            SYSLOG("UnifiedMemory", "PTE release route not verified; large buffer pool disabled");
        }
        return true;
    }
}

void X5000Memory::init()
{
    enabledAtBoot = checkKernelArgument("-NRedUnifiedMemory");
    if (!enabledAtBoot) { return; }
    PE_parse_boot_argn("nred-uma-wired-mib", &requestedBudgetMiB, sizeof(requestedBudgetMiB));
    backingLock = IOSimpleLockAlloc();
    if (backingLock == nullptr) {
        enabledAtBoot = false;
        SYSLOG("UnifiedMemory", "Could not initialize the residency registry");
        return;
    }
    lilu.onKextLoadForce(&kextIOAcceleratorFamily2);
}

bool X5000Memory::requested() { return enabledAtBoot; }

void X5000Memory::patchMetalBudgetPage(const char* const file, const UInt64 offset, const void* const page)
{
    const MetalBudgetSite* site = nullptr;
    if (offset == workingSetSite.pageOffset) { site = &workingSetSite; }
    else if (offset == segmentSite.pageOffset) { site = &segmentSite; }
    if (site == nullptr || strcmp(file, site->file) != 0 || strcmp(osversion, "25G332") != 0) { return; }

    auto* const bytes = const_cast<UInt8*>(static_cast<const UInt8*>(page)) + site->siteOffset;
    if (memcmp(bytes, site->original, site->length) != 0
        || MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS)
    {
        return;
    }

    if (memcmp(bytes, site->original, site->length) == 0) {
        memcpy(bytes, site->replacement, site->length);
        increment(metalBudgetPages);
        if (site == &segmentSite) { __atomic_store_n(&metalSegmentReady, true, __ATOMIC_RELEASE); }
    }
    MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
}

void X5000Memory::patchGLBudgetPage(const UInt64 offset, const void* const page)
{
    using namespace GLBudget;
    const bool shareGroup = offset == ShareGroupPage;
    if ((!shareGroup && offset != ResourceListPage) || strcmp(osversion, "25G332") != 0) { return; }

    auto* const bytes = const_cast<UInt8*>(static_cast<const UInt8*>(page))
                        + (shareGroup ? ShareGroupOffset : ResourceListOffset);
    const auto* const original = shareGroup ? ShareGroupFind : GLResourceBudgetPatch::Find;
    const auto* const replacement = shareGroup ? ShareGroupReplace : UnifiedReplace;
    const auto length = shareGroup ? sizeof(ShareGroupFind) : sizeof(GLResourceBudgetPatch::Find);
    if (memcmp(bytes, original, length) != 0
        || MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS)
    {
        return;
    }

    if (memcmp(bytes, original, length) == 0) {
        memcpy(bytes, replacement, length);
        __atomic_fetch_or(&glBudgetSites, shareGroup ? 1ULL : 2ULL, __ATOMIC_RELAXED);
    }
    MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
}

void X5000Memory::processKext(KernelPatcher& patcher, const size_t id, const mach_vm_address_t slide, const size_t size)
{
    if (!requested()) { return; }

    const Image image{patcher, id, slide, size};
    if (id == kextIOAcceleratorFamily2.loadIndex) {
        ioReady = verifiedImage(slide, size, ioAccelUUID) && installIOAccel(image);
        if (!ioReady) { SYSLOG("UnifiedMemory", "Unsupported IOAcceleratorFamily2 residency ABI"); }
    }
    else if (id == kextRadeonX5000.loadIndex && NRed::singleton().getAttributes().isRenoir()) {
        amdReady = verifiedImage(slide, size, x5000UUID) && installX5000(image);
        if (!amdReady) { SYSLOG("UnifiedMemory", "Unsupported X5000 display ABI"); }
    }
    activate();
}
