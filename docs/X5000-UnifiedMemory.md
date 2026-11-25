# X5000 Unified Memory (Renoir)

With `-NRedUnifiedMemory`, the Renoir accelerator places textures, IOSurface device caches and data buffers in
pageable system memory instead of the firmware carve-out (512 MiB on the test machine). Metal, OpenGL and
WindowServer keep using the normal system APIs. The code is in `NootedRed/X5000Memory.cpp`, `X5000Memory.hpp` and
`X5000MemoryPolicy.hpp`. The probes and the policy test are in `tools/`.

The feature is off by default. It installs only on devices whose `DeviceProfile` allows it (the Renoir family:
Renoir, Renoir/Green Sardine, Cezanne and Barcelo), and only when the loaded
`IOAcceleratorFamily2` (UUID `6C4A16B1-66DC-3A8B-9F0B-D3E027B16EB1`) and `AMDRadeonX5000` match macOS 26.7.2
(`25G332`). On any other build it logs `Unsupported ... ABI` and the native behavior stays in place.

## Placement

NootedRed hooks resource allocation in `IOAcceleratorFamily2` and GPU mapping in `AMDRadeonX5000`. Eligible primary
allocations get an `IOAccelSysMemory` backing of pageable system memory. X5000 reserves the GPU VA in the task's page
table and prepares (wires) the backing at submission. One placement policy covers native offscreen caches and
IOSurface device caches. It checks the task page table, the resource layout and the system memory descriptor.
IOSurface texture aliases follow the cache owner and keep the native CPU/GPU synchronization.

These allocations stay on the native carve-out path:

- direct scanout surfaces. `getDisplayPipeTransactionFlip` also refuses a system memory surface, because an offscreen
  IOSurface cache can reach a flip;
- external device layouts;
- allocations for which the kernel creates separate auxiliary descriptors, such as OpenGL MSAA and depth/stencil;
- fixed-address resources.

The native allocator also handles linear working textures on a connected VCN video context's share. This includes
its session buffer. Ordinary textures allocated through another share in the same process remain eligible for UMA.

DCC metadata that the Metal driver places inside a normal texture allocation, and reaches through the same GPU VA,
pages together with the texture.

OpenGL linear vertex buffers can also use a pageable primary allocation. The driver imports a native system memory
descriptor through `newResourceWithClientBuffer`. The policy checks that the resource has no texture format or
layout, comes from a client buffer, belongs to the same GPU task, and that the task has a per-task page table. The
native upload and synchronization code then writes the data into the unified memory primary.

When unified memory installs on AMDRadeonX5000, special allocations keep the native VRAM allocator and the extended
`allocateLargeBlocks` is not installed. If it does not install (unsupported build or device), the extended allocator
is installed as without the boot argument. The existing OpenGL low-VRAM budget fix stays as the fallback.

## Residency and reclaim

When the residency budget is reached, the native LRU reclaims only system memory that has no pending prepare
reference and whose GPU events have completed. `IOAccelSysMemory::unwire` removes the PTEs, keeps the mapping object
and the GPU VA, and unwires the pages. A later submission maps the same backing back to the same GPU VA.
Resource-level `pageoff` involves backing migration and blits and is tested separately.

For purgeable requests the native resource methods first handle the retained state and the cache valid bits.

- `Empty` asks for an immediate discard. When the backing has no prepare reference and all GPU events have
  completed, unified memory calls the native `unwire`, which removes the PTEs and releases the pages
  (`purge_unwires`).
- For a Metal `Managed` buffer or texture, an explicit `Volatile` request can release the wires on both the
  CPU copy and the GPU primary. The CPU map must belong to the allocating task and map the backing descriptor.
  Both copies must be idle, including any inherited pool event. Each copy must have just the resource and its own
  task mappings as references, with a wire count matching its committed mappings. The checks run for both copies
  before the first `unwire`. The native method then applies the recorded state to the descriptors. The VM can
  reclaim their pages and the GPU keeps its address reservation.
- The native resource method maintains the validity of each copy. During a query or a transition out of a
  purgeable state, the hook also observes the two native memory results on the calling thread. If either copy
  reports `Empty`, Metal reports `Empty` for the pair so the application can recreate its contents.
- Other `Volatile` requests follow native LRU reclamation. This includes WindowServer surfaces, which are often
  used again immediately during window and full-screen transitions.

In `25G332` the hook sits between `lock_busy` and `unlock_busy` in `set_resource_purgeable`. The native non-blocking
LRU path also checks the prepare count and the GPU events before `unwire`.

## Budgets

Metal clients get three quarters of the wired limit as the recommended working set, and half of that as the command
segmentation threshold, so several command buffers and the system graphics clients share the headroom. OpenGL gets
its command resource list budget, half of the recommended working set, through the same device config interface.
Both budgets are published only for the Renoir device with unified memory on. The physical carve-out size is
reported separately. An OpenGL device that does not receive a budget keeps the existing compatibility policy.

The drivers read these budgets through four patched sites: the recommended working set and the command segment limit
in the Metal driver (dyld shared cache), and the share group and resource list setup in the OpenGL driver. Each site
is a `SignaturePatch` that `UserPatches` applies with its signature matcher and records for re-application when the
page is read again. A site is applied only when its whole original code matches once on its page, and the list stays
empty until the kernel routes are installed. The resource list site replaces the same leaf as the low-VRAM GL budget
fix and uses that fix's formula when no budget is published.

## Page tables

Page tables stay in the carve-out. `AMDHWVMM` allocates them and SDMA updates them. The native writer emits `PTE_PDE`
packets per physical range. Scattered 4 KiB pages therefore generate many single-entry packets.

A CPU-written leaf page table was tried and dropped. The asynchronous clear in `AMDHWVMContext::clearWithDMA`
overwrites CPU writes that finish earlier, so every leaf write would have to come from the CPU. The GPU does not
snoop the CPU cache for page tables, so write-back leaf pages need a flush per cache line. With the flushes the
barrier wait fell to 0.06 ms, but WindowServer kernel CPU time per run rose from 6.2 s to 11.7 s and long frames in
the light animation from 16% to 34%. A write-combined mapping caused GPU page protection faults. Linux amdgpu also
updates the graphics page tables of Renoir with SDMA.

Runs of single-page PTE updates are coalesced for unified memory backings. Other GPUs driven by `AMDRadeonX5000`, such
as a Vega card next to the APU, keep the native writer. Inside `AMDHWVMContext::mapVA`, single-entry updates of
consecutive PTEs from `mapVMPT` are staged and written as one SDMA `WRITE_LINEAR` packet with two dwords per entry, as
in amdgpu's `sdma_v4_0_vm_write_pte`. The thread flushes its staged entries before any other reservation or commit on
the same command pool and when the outermost `mapVMPT` returns, so the pool holds every packet when `mapVA` ends. If a
large reservation fails, the entries go to the native single-entry writer. SDMA stays the only page table writer, and
`unmapVA` keeps the native path. A 64 MiB texture needs about 190,000 dwords of PTE packets. The GPU job wait on the
page table barrier drops from about 11 ms to 2-3 ms. Counters: `pte_batches`, `pte_batched_entries`, `pte_fallbacks`.

### 64 KiB fragments

`-NRedUMA64K` enables an experimental path for pageable Renoir backings. At normal pool pressure a clean allocation
can inspect up to eight wired descriptors per eligible bucket. It swaps an otherwise unused candidate into the
allocation when its first physical segment contains a 64 KiB-aligned, contiguous window and the selected piece
lacks one. Dynamic task VA allocations of at least 64 KiB try 64 KiB alignment when the resource asks for a smaller
alignment. A failed reservation retries with the original alignment. Fixed reservations follow the native allocator.

On each eligible commit, the driver reads the current GPU-visible address and length from `IOAccelSysMemory`.
For a direct physical mapping it can join adjacent segments from the same backing after verifying all sixteen pages
of one aligned window. DMA commands and system-mapped addresses keep their native segment boundaries. A shorter
span also limits the fragment to the alignment shared by the physical address, absolute GPU VA and VA relative to
the VM context base. `mapVMPT` chooses
the order and `AMDGFX9VMM::getPTEValue` encodes it in PTE bits 7–11. SDMA submission, invalidation and unmap follow
their existing paths. Each rewire checks the new layout. In `25G332` the XNU pageable descriptor factory creates a
VM memory entry. Its separate physical allocation path cannot supply a pageable, purgeable 64 KiB guarantee.

`-NRedUMAFragmentStats` enables observation alone. `-NRedUMA64K` includes the same measurements. The leaf counters
cover successful managed mappings observed from start to finish by the coalescing thread.

| Counter | Meaning |
| --- | --- |
| `fragment_windows` | Complete VA-aligned 64 KiB windows in the sample |
| `fragment_contiguous_windows` | Windows whose sixteen GPU-visible pages also start aligned and increase consecutively |
| `fragment_mapped_windows` | Complete windows written with native PTE fragment order 4 or greater |
| `fragment_4k_pages` | Individual pages written with order 0. Orders 1–3 have intermediate sizes |
| `fragment_incomplete_maps` | Owned `mapVA` calls excluded because they failed or their leaf coverage was incomplete |
| `fragment_pool_swaps` | Additional eligible pieces brought into the allocation by clean pool selection |
| `fragment_va_retries` | Preferred VA reservations retried at the original alignment |

Divide the *delta* of `fragment_mapped_windows` by the delta of `fragment_windows` to measure encoded coverage over
a workload. A zero denominator means no usable windows were sampled. The contiguous fraction measures the available
opportunity. Successful fragments can reduce translation work on dependent page reads. Repeated segment queries,
alignment retries and pool selection can increase map or allocation time when the workload has little contiguity.

## Mapping reuse

A committed `IOAccelMemoryMap` holds a wire reference on its backing until `release_pte`. WindowServer keeps deleting
and re-creating resources that refer to the same IOSurface cache. A task has one map per backing, shared by the
resources that use it. Deleting the last resource freed the map, cleared the PTEs and unwired the backing. Tens of
milliseconds later the next resource allocated a new map and GPU VA, wired the backing again and rewrote every PTE.
In a 25 s trace on 2026-11-22, 3043 of 3228 unwires came from this.

`IOAcceleratorFamily2` already has a mechanism for it. After `IOAccelMemory::setFlags(2)`,
`IOAccelMemoryMap::release` moves the task's last map, with its GPU VA and committed PTEs, to the task's orphan
list. `createMappingInTaskAtAddressLength` revives it when the same task maps the same backing again, and `prepare`
skips the commit while the PTEs are still valid. The AMD driver does not use this. Unified memory enables it for the
primary backings of textures, surface caches and data buffers. Without the flag, the native code already orphans
maps whose GPU work has not finished.

Orphans keep their wire reference and are reclaimed by the native paths. The GART collector runs every 250 ms and
unwires backings idle for 20 s, 10 s or 5 s once total wired memory exceeds 32 MiB, 1/16 or 1/8 of RAM. `unwire`
clears the PTEs of every map of the backing, orphans included. The map objects and the GPU VA remain until they are
revived or freed. When a task runs short of GPU VA, `freeWaitToAllocGPUAddress` frees orphans first. Orphans of a
long-lived backing have no separate count or age limit.

Two native paths need extra handling:

- A pooled backing keeps its descriptors while any orphan map survives the event-checked prune. The native orphan
  collector owns the remaining lifetime. `pool_busy_orphans` counts these deferrals. This also covers maps that are
  still busy when the owning task exits.
- For a fixed-address request on a managed backing, each same-task candidate must have a live fixed reservation at
  the exact VA and length. A different or busy orphan at another VA is skipped during the search. The native allocator
  gets a chance to reserve the requested range. After a failed reservation the driver prunes idle orphans and retries
  once, and validates the returned reservation. Fixed-address resource placement itself remains native.

On 2026-11-22 (kext `8C62ED5A`), compared with the unified memory build without mapping reuse: PTE commits per
animation run fell from 4741 to 1686, WindowServer page table command buffers from 33758 to 4519 (median length 4080
to 602 dwords), and the median scheduler hold of graphics submissions in the light animation from 6.47 ms to
0.01 ms (p90 18.5 ms to 0.50 ms).

### Private texture cache

`-NRedUMATextureCache` enables an experimental cache for repeated allocations of standalone private textures. The cache
holds one backing reference and its detached, committed task mapping. On a match that reference transfers to the new
resource. The key includes the process task, `IOAccelShared2`, GPU task, page table, backing options, size, alignment,
format, geometry and plane layout. Eligibility requires a single orphan mapping, no attached resource, CPU mapping,
external event or outstanding prepare, and completed GPU events. An incompatible mapping request skips the saved map
and can allocate a fresh VA. Buffers, IOSurfaces, multi-plane textures and resources with auxiliary storage use their
existing allocation paths.

There are 16 slots per device. Each slot expires after two seconds and is collected on pool maintenance or lookup.
The byte limit is `min(wired_budget / 32, 256 MiB)`, with at most half that limit per process task. Pressure, shared
teardown and device stop release retained backings. Channel restart and system sleep advance `reuse_generation`.
Reuse resumes after the native wake completes and requires the current generation. `texture_cache_stores`,
`texture_cache_hits` and `texture_cache_evictions` count admissions, backing transfers and discards.
`texture_cache_mapping_hits` counts compatible transfers that actually revive a committed mapping.

## Native buffer pool

For GPUs that use system memory as primary storage, `IOAcceleratorFamily2` provides
`IOAccelBufferMemoryDescriptorPool2`. `IOGraphicsAccelerator2::start` creates it only when bit 21 of the driver
config dword at `0xc90` is set, and the AMD driver does not set it. Unified memory creates the pool for the Renoir
accelerator at the same check without changing the config bit, so the other native behavior tied to that bit stays
unchanged.

### Buckets

The pool keeps wired, pageable, purgeable descriptors in five buckets. At normal pressure the pool timer targets 24
descriptors per bucket. The first bucket size times its current refill target sets the request cap. The
native first bucket is 1 MiB, so its normal cap is 24 MiB. WindowServer's 10-bit full-screen half-float textures are
25-64 MiB.

On machines reporting at least 12 GiB of RAM (firmware can subtract the carve-out from a 16 GB configuration),
with a default budget or an override of at least 4 GiB, the Renoir pool selects 4 MiB, 1 MiB, 256 KiB, 64 KiB and
4 KiB buckets. `initWithAccelerator` selects the shift and capacity for that pool before allocating its arrays. Other
accelerators read the original tables. Installation verifies the table values and loads, prefill, request cap and
native stack-array size. With a normal refill target the request caps are 96 MiB and 24 MiB. Exhaustive page-sized
policy tests give maxima of 47 and 35 descriptors respectively for the 64-entry array. Unaligned and oversized
managed requests go directly to the pageable factory. The native timer refills and zeroes descriptors under the
accelerator lock.

### Hand-back

Natively, descriptors return to the pool only from `IOAccelSysMemory::orphanIt`. Pooled backings almost always end
in `IOAccelMemory::release` instead: once the mapping events complete, it calls `pruneOrphanedMappings` and frees
the backing. Unified memory takes over that call site (`release+0xa6`) and its own `orphanIt` route:

- it adds a wire reference before the prune, so the last `release_pte` does not unwire the whole descriptor set;
- it samples the native page table update event of the `AMDHWVMContext` whose mappings were released (`context+0x40`);
- when the backing is destroyed, it calls `releaseDescriptors` as `orphanIt` does and returns the descriptors with
  that event, keyed to `kernel_task`. The native timer waits for the event and zeroes the descriptors before any
  allocation can reuse them. This avoids transferring unfinished PTE updates from the dying backing to a new
  allocation on the same task.

The default allocation path requests clean pieces. With fence reuse enabled, only an eligible private texture can
consume same-task dirty pieces after verified retirement. Other native pool returns, including allocation rollback,
use the kernel key in this mode. Shared release rekeys old task slots. The first dirty lookup or verified return in
a new device generation also rekeys stale slots under the pool lock.

An `Empty` request on a pooled backing also unwires at once. The native code drops emptied descriptors when they
return to the pool.

### Footprint

`releaseDescriptors` keeps pieces up to the combined wired and purgeable bucket capacities. Normally the refill
target is 24. At XNU warning or urgent pressure, or at 7/8 of the accelerator wired budget, the target drops to four.
At critical pressure or above, or at the wired limit, it drops to zero. The driver reads the internal XNU pressure
level from XNU's `bsd/sys/event_private.h` (normal 0, warning 1, urgent 2, critical 3). The accelerator budget
thresholds also work independently.
The native request cap follows the first bucket's target.
The locked GART collector and pool timer release up to four excess clean wired descriptors per bucket per visit,
yielding to accelerator waiters. During a pressured refill the cleaner uses the same wired capacity as the target.
This allows it to retire dirty slots into purgeable storage or release them. The original capacity is restored
before leaving the native refill. A recovering target below its desired occupancy restarts the refill timer.

| | Wired at the refill level | Kept at most (wired + purgeable) |
| --- | --- | --- |
| Large buckets (16 GB and more) | about 128 MiB | about 200 + 200 MiB |
| Native buckets | about 32 MiB | about 75 + 70 MiB |

The 4 MiB bucket has 32 wired and 32 purgeable slots. The following measurements predate pressure trimming. On the test machine
after a day of use, `pool_state` read `29/32/0 64/52/0 32/6/0 32/0/0 22/12/0` (wired/purgeable/dirty per bucket):
about 190 MiB wired and 180 MiB purgeable. Right after the 23 GiB pressure probe the large-bucket pool still held
about 200 MiB wired (`32/32/0 64/24/0 32/12/0 32/16/0 24/0/0`). With the native buckets it held about 75 MiB wired
(`64/32/0 32/16/0 30/16/0 30/7/0 32/4/0`) after a 5 GiB reclaim run at a 4 GiB budget. `bytes_total_wired` does not
count idle pool descriptors. `bytes_managed_wired` is the managed subset of `bytes_total_wired`. The most recent
pool sample reports clean wired, clean purgeable and pending dirty bytes in `pool_clean_wired_bytes`,
`pool_purgeable_bytes` and `pool_pending_bytes`. Purgeable bytes are descriptor lengths and may already have been
reclaimed by the VM. `texture_cache_bytes` is part of `bytes_live` and overlaps wired accounting while resident.
`pool_trimmed_bytes` is cumulative and `pool_refill_target` reports the last maintenance decision.

### Effect

Before the pool, each window cache create or destroy allocated pageable memory, faulted and zeroed it page by page,
wired it, and unwired it on release. In a kernel stack sample on 2026-11-22, wire and unwire took 15.5% and 13.5% of
WindowServer kernel CPU time. Wiring a new large texture happens under the accelerator lock. In a 64 MiB and 16 MiB
create/release loop, WindowServer was blocked on that lock for 151 ms in total (longest 26.6 ms). With the pool and
hand-back it was 17 ms (longest 3.1 ms).

If any route of the pooled path (the shared release, prune or PTE release route) cannot be verified, no pool is
created and every backing takes the pageable path. Natively returned descriptors are keyed by task, and without the
re-keying a later task at the same address could receive them.

Counters: `pool_allocations` (backings served by the pool), `pool_returns`, `pool_return_pieces`,
`pool_return_skips` (dying backings the hand-back checks declined; they are freed the native way),
`pool_large_requests` (requests above 24 MiB), `pool_rekeyed` and `pool_state`.

`pageon` keeps the driver's native VRAM staging when it needs a temporary copy. A pageable staging route was
removed, because the native staging branch was never reached during the animation benchmark and all probes.

## Configuration

Add `-NRedUnifiedMemory` to the boot arguments. Lilu must load before NootedRed. The large pool buckets, hand-back
and PTE coalescing are on by default with it. To roll back, remove the argument.

- Each part acts only after it has verified that its routes are in place. Otherwise it turns itself off and keeps
  the native behavior. There are no runtime switches.
- `-NRedUMAFenceReuse` enables experimental same-task descriptor reuse for pooled textures. It requires the
  verified IOAcceleratorFamily2 pool and X5000 VMPT routes. With `-NRedUnifiedMemory` alone, allocations use clean
  descriptors and hand-back uses the kernel task key.
- `-NRedUMATextureCache` independently enables reuse of a private texture backing and its task mapping. It requires
  verified pool, mapping, restart and power hooks and the XNU pressure signal.
- `-NRedUMA64K` enables fragment-aware selection, VA allocation and mapping. `-NRedUMAFragmentStats` on its own
  collects the comparison sample. `texture_cache`, `pool_fence_reuse`, `fragment_64k` and `fragment_stats` report
  which options installed.
- The GPU's registry entry (`IGPU` in `ioreg`) shows the state. `NRedUnifiedMemory` reads `active`, or why the
  feature is off (`off: unsupported IOAcceleratorFamily2`, `off: unsupported AMDRadeonX5000`, `off: device not
  supported`). `NRedCompatUnifiedMemory` is `1` when the device profile allows unified memory. While active,
  `NRedUnifiedMemoryPoolBuckets` is `large` or `native`. `NRedUnifiedMemoryHandBack`,
  `NRedUnifiedMemoryPTECoalescing`, `NRedUnifiedMemoryTextureCache` and `NRedUnifiedMemory64KFragments` report
  their individual states.
- Once AMDRadeonX5000 is processed, NootedRed logs one line in the format of the other `Compat:` lines:
  `Compat: feature=UnifiedMemory profile="Renoir" level=experimental state="active" buckets=large handBack=1
  pteCoalescing=1`.
- The read-only `debug.nred_uma` sysctls report the state and the counters the probes use. `active` is `1` once the
  driver routes are installed. `pool_state` reads `none` until the driver samples the pool under the accelerator
  lock. Allocations, hand-back and maintenance update the sample. `pool_large_requests` counts eligible requests
  above 24 MiB, `pool_returns` counts hand-backs and `pte_batches` counts coalesced packets.
  `allocation_failures` counts pageable backings that could not be allocated, and
  `scanout_denials` counts system memory resources refused for direct scanout.
- NootedRed also logs `Creating the native buffer pool` and `Pooled hand-back on, buffer pool buckets large`, but on
  the test machine these lines did not reach the unified log.
- `nred-uma-wired-mib=<MiB>` lowers the residency budget, for example `768` to trigger native reclaim in tests.
  Below `4096` it also keeps the native pool buckets.

## Testing

Record for every run: the macOS build, the source commit and any local changes, the kext UUID from
`kmutil showloaded`, the boot arguments, and `sysctl debug.nred_uma` before and after.

Build the Metal probe and run a small readback from the repository root:

```sh
xcrun clang -fobjc-arc -O2 -Wall -Wextra -Werror \
  -framework Foundation -framework CoreGraphics -framework IOKit -framework IOSurface -framework Metal \
  tools/x5000_uma_probe.m -o /tmp/x5000_uma_probe
codesign -s - /tmp/x5000_uma_probe
sysctl debug.nred_uma
/tmp/x5000_uma_probe --groups 1 --require-budget --require-surfaces --surface-ipc --layouts
```

- The probe reads the carve-out size from the `VRAM,totalMB` property on the GPU (512 MiB if it is missing). Each
  group holds 32 MiB, so pick `--groups` above carve-out / 32 MiB; the probe prints a note when the working set
  still fits in the carve-out.
- Capacity: with a 4K desktop active, `--groups 36 --require-budget` (for a 512 MiB carve-out).
- Reclaim: boot with `nred-uma-wired-mib=768` and run
  `--groups 36 --idle 10 --require-budget --require-reclaim --require-surfaces --surface-ipc`. The probe keeps
  private buffers and render targets, checks that `bytes_live` grows by more than the carve-out and verifies the GPU
  readback byte by byte. Reclaim also requires `same_va_recommits` to grow and checks the buffer GPU VAs.
- `--require-surfaces` checks the pageable cache counters. `--surface-ipc` maps one IOSurface into two processes
  before the pressure, keeps the textures in both until the pressure ends, and checks GPU writes and CPU rewrites
  from both sides.
- `--layouts` covers private heap buffers, color and depth textures, and a 4x MSAA resolve with depth and stencil
  attachments.
- `--managed` uses `MTLStorageModeManaged` buffers and textures: CPU write, GPU readback, CPU sync after a GPU write,
  and a second upload into the same resource.
- `--purgeable` checks Metal purgeable states and their recovery, requires a rewrite and readback at the same buffer
  GPU VA after `Empty`, and requires `purge_unwires` to grow. AMD Metal turns a `Volatile` request on a private
  render target into `NonVolatile`; the probe then checks that the contents were kept.
- `--managed --purgeable` synchronizes both resources before the request. On a UMA boot the probe also queries the
  VM object at the buffer's CPU address while volatile and after restoration. It records the calling task's
  graphics footprint during each request and checks each retained resource on both the CPU and GPU.
- `--seed` sets the first check byte, so parallel processes can use different data to check task isolation at the
  same GPU VA.
- `--hotplug 300` keeps the resources while the main external display goes offline and comes back at its original
  resolution, then reads them back. Combine it with `--surface-ipc` to cover IOSurfaces mapped into two processes
  before the reconnect. Check on the panel that the picture was visible before unplugging and that the desktop
  comes back and updates.
- `--single-dispatch` (with `--groups 36` or more) reads 36 render targets (576 MiB) in one compute dispatch, so a
  single command references more than a 512 MiB carve-out.
- On a boot with unified memory off, `--baseline --groups 1 --managed --purgeable` checks Metal's native
  purgeable-state transitions and readback. The Metal and OpenGL probes reject `--baseline` if UMA is active.

The driver counters are per device and include other processes on the same device.

The OpenGL probe samples five textures in one draw, which covers a command larger than the old resource list
budget and several resource groups. The full case keeps groups of 272 MiB, at least three and at least 256 MiB more
than the carve-out (816 MiB for 512 MiB). It requires both budget hooks, the new device budget, a completed GPU
readback and more pageable primary backing than the carve-out. `--smoke` uses smaller textures.

```sh
xcrun clang -std=c17 -DGL_SILENCE_DEPRECATION -O2 -Wall -Wextra -Werror \
  -framework CoreFoundation -framework IOKit -framework OpenGL tools/x5000_uma_gl_probe.c -o /tmp/x5000_uma_gl_probe
codesign -s - /tmp/x5000_uma_gl_probe
/tmp/x5000_uma_gl_probe --smoke
/tmp/x5000_uma_gl_probe --layouts
/tmp/x5000_uma_gl_probe --buffers
/tmp/x5000_uma_gl_probe
```

`--buffers` writes nine 64 MiB VBOs and reads back their first and last markers. With `--wait-for-pressure` the
probe keeps the resources and reads them again after an empty line on standard input. The VBO case then updates the
buffers from the CPU and checks that the GPU sees the update.

The churn tool writes a private RGBA16Float texture and verifies a tagged pixel at a different location each
iteration. It records the first wall time separately. After five warm-ups it reports wall and GPU P50/P95 over
24 iterations plus device counter deltas.
It prints the installed options separately from changes in mapping, pool, cache and fragment counters.
The default allocates a fresh texture each time. `--compare` also tests a reusable heap and a persistent texture,
then repeats the three modes in reverse order. `--async` verifies 48 fresh textures using two queues with up to
three submissions in flight. Size arguments are nominal MiB for square textures.

```sh
xcrun swiftc -O tools/x5000_uma_churn.swift -o /tmp/x5000_uma_churn
codesign -s - /tmp/x5000_uma_churn
/tmp/x5000_uma_churn 64 16
/tmp/x5000_uma_churn --compare 64 16
/tmp/x5000_uma_churn --async 64 16
```

With `-NRedUnifiedMemory -NRedUMAFenceReuse`, check `sysctl -n debug.nred_uma.pool_fence_reuse` for `1`.
Only pooled textures whose last prune retired every remaining mapping with a verified event can return with
their original task key. For each release, the driver checks for a queued SDMA invalidate, submission from the
outermost update, and a changed context event matching the command pool's stamp. If the native event merge dropped
every completed stamp, the return retains the last verified one for the allocation check. Earlier unverified
releases, native `orphanIt`, and allocation rollback use the kernel key. A same-task allocation skips a pending
slot and keeps looking. Once the event completes, it can take the slot and strip the retired dependency before
the native allocator merges events into the new backing. The pool timer waits and zero-fills pieces for cross-task
reuse. Shared release rekeys any pieces that retain the old task identity.

Read the reuse counters before and after the churn test.

```sh
sysctl debug.nred_uma | awk '/pool_fence_reuse/'
```

`pool_fence_reuse_returns` counts backings returned with the original key. `pool_fence_reuse_hits` counts
descriptors actually taken, and `pool_fence_reuse_pending` counts checks that rejected a slot. For a useful
performance comparison, boot the same build with and without reuse, keep the displays and workloads
matched, and check that the hit count increases during 64 MiB and 16 MiB churn before attributing a timing change
to reuse. Run the hardware encoder alongside texture turnover and exercise normal asynchronous Metal submissions
and task teardown. The fast path
changes descriptor selection. GPU virtual addresses and page tables still use the native allocators.

The cross-process check writes marked 64 MiB private textures in one process and looks for the marker in new
textures of another process. The `both` run is the control. It needs visible same-task reuse to show that this
test actually exercised dirty descriptor reuse. Check the hit counter as well.

```sh
xcrun swiftc -O tools/x5000_uma_leakcheck.swift -o /tmp/x5000_uma_leakcheck
codesign -s - /tmp/x5000_uma_leakcheck
/tmp/x5000_uma_leakcheck both 4    # expect SAME_TASK_REUSE_VISIBLE above 0
/tmp/x5000_uma_leakcheck write 8
/tmp/x5000_uma_leakcheck read 8    # expect READ 8 with_pattern=0
```

With the native pool buckets (machines below 16 GB), 64 MiB textures do not come from the pool, so pass `16` as a
third argument to all three runs.

Three Metal workloads compare unified memory with the carve-out under load. Run each in a boot with and a boot
without `-NRedUnifiedMemory`:

```sh
for t in workingset timeline export; do
  xcrun swiftc -O tools/x5000_uma_$t.swift -o /tmp/x5000_uma_$t && codesign -s - /tmp/x5000_uma_$t
done
/tmp/x5000_uma_workingset 256 512 1024 2048 3072   # resident textures, GPU read rate per size
/tmp/x5000_uma_workingset --latency 64             # random dependent loads, 4 KiB and 64 KiB strides
/tmp/x5000_uma_timeline 8 12 120 medium             # 4K layers, blur + grade into a frame cache (1.3 GiB)
/tmp/x5000_uma_export 10 60 /tmp/x5000_uma_export.mov large   # Core Image effects + hardware HEVC, then decode
/tmp/x5000_uma_workingset --hold 1024 150           # keeps 1 GiB resident while another test runs
```

For each working-set size the probe holds the requested payload, including a partial final texture for sizes
outside multiples of 32 MiB. Every read pass checks an independent GPU result for each lane against its expected
value on the CPU. P50/P95 and payload GiB/s cover submission through GPU completion. CPU verification falls
outside that interval. Hold mode repeats the same check throughout the hold.

The latency mode constructs one deterministic random cycle in a private Metal buffer per stride. A single GPU
thread follows that chain. Each dispatch starts at a different node and the CPU checks the final link. Three warm-ups
precede twenty GPU timings. Compare the same stride and working set across boots. Different strides also change the
number of nodes and their cache footprint.

`tools/bench_x5000_uma.py` runs the four compiled probes once per boot. Give it the directory containing
`x5000_uma_churn`, `x5000_uma_workingset`, `x5000_uma_probe` and `x5000_uma_gl_probe`. Keep these binaries on disk
across boots so their recorded hashes match. `--kext` checks the loaded UUID before running any workload.

```sh
python3 tools/bench_x5000_uma.py "$probe_dir" --kext "$candidate_kext"
```

For a comparison on the same release, use these boot arguments in addition to the usual system arguments.

| Run | UMA arguments |
| --- | --- |
| Native purgeable control | None |
| Default UMA | `-NRedUnifiedMemory` |
| Fragment measurement control | `-NRedUnifiedMemory -NRedUMAFragmentStats` |
| Private texture cache | `-NRedUnifiedMemory -NRedUMAFragmentStats -NRedUMATextureCache` |
| Fence-gated descriptor reuse | `-NRedUnifiedMemory -NRedUMAFragmentStats -NRedUMAFenceReuse` |
| 64 KiB selection and mapping | `-NRedUnifiedMemory -NRedUMA64K` |

The runner saves the display configuration, boot session, probe hashes, loaded UUID, timings, readbacks, counters
after each workload and the exit status of each check in `status.tsv`. Compare `task_commits` and
`texture_cache_mapping_hits` during fresh churn, and `fragment_mapped_windows` and `fragment_windows` during the
page-latency run. The native boot runs the same Managed and purgeable check using the probe's `--baseline` mode.
For a matched reclaim experiment on UMA boots, use the same lowered wired budget and pass `--pressure-groups N`
with a working set exceeding that budget. The runner records the driver counters before reclaim, on completion
and after 30 seconds idle, then repeats fresh churn. Compare the first allocation and warm P95 together with
`bytes_managed_wired`, `pool_clean_wired_bytes`, `pool_trimmed_bytes` and the refill target.

`python3 tools/test_x5000_memory_policy.py` checks placement, residency, budgets, both bucket tables at each
refill target, fixed reservations, bounded clean pool selection, VA fallback and physical segment replay on the host.

## Results

Test machine: Renoir `0x1636`, 64 GiB RAM, 4K 10-bit DisplayPort display, macOS 26.7.2 (`25G332`).

### November 25, 2026

The running kext was `39E31418` with UMA and fence reuse enabled. The updated probes completed 48 asynchronous
readbacks each at 64 and 16 MiB, checked working sets of 1, 33, 256 and 1024 MiB and exercised hold mode.
Metal private resources, cross-process IOSurface, layouts/MSAA, Managed resources alone and OpenGL smoke passed.
With `--managed --purgeable`, the buffer and texture both reported `NonVolatile` after requesting `Volatile`.
`Empty`, rewrite at the original buffer VA and readback passed. The combined state check failed. A matching
UMA-off run is still needed for that case.

At 15:04:08 Shanghai time a `swift-rdp` XCTest initialized the 1920×1080 hardware H.264 encoder after its
low-latency selection failed. From 15:04:15 through 15:07:19, seven GPU reset reports identified channel 14
`VCN0EncLLQ`. All seven reported the same pending `VTEncoderXPCService` submission, `0x97e`, with `0x97d`
completed. The first report also recorded an MM-hub read protection fault on VMID 10. X5000 looked up VMID 8
in the encoder task's context while writing the report. The VMID and IB length submitted for `0x97e` are unknown.
After two watchdog recovery attempts and
120 seconds without a WindowServer check-in, `watchdogd` triggered a kernel panic. The next boot started at 15:08:18.
Earlier that day, the same XCTest encoder configuration at 07:48:08 preceded another reset of channel 14 at
07:48:16. Both first reports recorded MM-hub read faults from client 29. The morning fault was on VMID 11
at `0x400010000`, the afternoon fault on VMID 10 at `0x40000c000`. The morning boot had `-NRedUnifiedMemory`
alone. The afternoon boot also had `-NRedUMAFenceReuse`.

The UMA review build (`BB367DF1-D18B-3C42-B3F8-F60E75A1008C`) compiled in Release with matching kext and dSYM
UUIDs. The offline checks matched the pool tables, the fragment route instruction signatures, the leaf update call
and four virtual dispatch targets in `25G332`. The host policy suite passed with ASan and UBSan. In its 128 KiB
replay, two aligned, physically consecutive 64 KiB windows split at 4 KiB segment boundaries went from zero
encoded windows to two. Rewiring the same logical pages in reverse order yielded 32 base-page PTEs.

The historical `churn_only` trace in `build/uma-bench3/contig.out` contains 508,553 nonzero-address updates with
a 4 KiB stride, spanning 509,384 entries. Across updates where both the PTE destination and physical page increased
consecutively, the longest run was ten pages. No sixteen-page window appeared in this sample. The trace omits GPU
VAs, PTE flags and unused pool candidates, so it cannot predict hit rate after pool selection or a timing gain.

The same day a separate run on the loaded UMA-only kext `CB2FC334` used the final probes. `system_profiler` listed
the Renoir GPU and no attached display. Its results are recorded in `build/uma-profiles/cb2fc334-final-probes-20261125`.

| Workload | Loaded UMA-only reference |
| --- | --- |
| Fresh 64 MiB texture, two 24-sample runs | Wall P50 19.19 / 19.50 ms, P95 20.14 / 22.31 ms |
| Fresh 16 MiB texture, two 24-sample runs | Wall P50 5.52 / 5.32 ms, P95 6.19 / 6.59 ms |
| 64 MiB random reads, 4 KiB / 64 KiB stride | GPU P50 334.5 / 169.4 ns per load, P95 345.2 / 220.2 |
| 256 MiB / 1 GiB texture reads with full-payload checksums | 29.69 / 32.61 GiB/s |

Two-queue churn verified 48 readbacks at each size. Private purgeable resources, cross-process IOSurface, Metal
layouts, Managed readback and OpenGL smoke passed. The combined Managed and purgeable check again observed
`NonVolatile` after a `Volatile` request on both resources. Discard, rewrite and readback passed at the original
buffer VA.

The combined candidate `9704C1C3` booted at 19:08:24 Shanghai time with UMA active. Six of the seven UMA
checks passed. The Managed buffer and texture again reported `NonVolatile` after a `Volatile` request.
Both reported `Empty` after discard, and the refill and readback passed at the original buffer VA. Three repeats
with the updated probe on `9704C1C3` found that the Managed buffer's CPU VM object stayed nonvolatile and
the task's graphics footprint changed by zero bytes for each 16 MiB resource during `Volatile`.
A run with UMA disabled on this machine would establish how the native driver handles the `Volatile` request.

With the same working-set probe binary, OS build, boot arguments and display configuration as `CB2FC334`,
the 256 MiB and 1 GiB reads measured 30.55 and 33.01 GiB/s, respectively, up from 29.69 and 32.61 GiB/s in
the reference run. Median dependent-load latency at 4 KiB and 64 KiB strides measured 334.2 and 169.0 ns,
against 334.5 and 169.4 ns. Each build contributed one benchmark run.

On `A09487DF` all seven UMA workloads passed. In three additional Managed runs, both the 16 MiB buffer
and the 16 MiB texture became `Volatile`. Each reduced the task's graphics footprint by 16 MiB. After
discard, refill and GPU readback succeeded at the original buffer VA. The 256 MiB and 1 GiB working sets
measured 30.83 and 32.20 GiB/s in one run. The median 4 KiB and 64 KiB stride latencies were 335.0 and
168.5 ns per load.

### SwiftRDP and VCN completion

The VCN submission repair targets Renoir `0x1636` with the `25G332` X5000, X5000HWLibs and X6000 driver UUIDs.
It uses the existing VCN bridge's ABI results and binds to the VCN2 engine on NootedRed's PCI device at
initialization. The engine's release ends that binding. The video repair is independent of the UMA boot argument.
During the November 25 incident, the WindowServer watchdog sample placed an I/O termination thread in
`contextStop → finishAllStamps → restart_channel → resetHardwareAndReplay → waitForTimestamp`.
The native reset also copies the submitted stamp into timestamp memory. A hardware timestamp observed after
reset cannot establish whether the original job executed.

X5000 sets flag `0x2` when it discards a replay. Native X6000 commit returns the requested stamp without
submitting a frame for this flag. In NootedRed 1.3.4 the upstream `submitCommandBuffer` hook retires the
skipped job after the native call returns. The PR forwards flagged jobs to native commit, including during
recovery from a queue error. This leaves one writer responsible for retiring a discarded job.

For an ordinary zero-length primary IB with the flag clear, native commit submitted the initial template with
its original fence value. For a populated primary IB it also returned the requested stamp after a TTL enqueue
error. For empty jobs the PR queues a frame containing the channel's fence address, requested timestamp, trap
and end command. It replaces the IB with native NOPs and preserves the decoder start packet.
If a zero primary IB has live auxiliary IBs, the native TTL builder encodes them in order in one frame with a
single final fence. An active primary keeps native video packet selection even if side buffers are present.
All three paths check the TTL operation results. After the first failure, later submissions on that queue return
the same preceding stamp until native reset. Healthy queues can continue independently. On a normal frame the
GPU receives the original packet sequence. The CPU runs one IB query and build, writes the stamp into the copied
fence packet and records a short history entry. The original commit also queried and built the fence packet.

`NRedVCNOrderedFence = 1` on the iGPU records activation. The diagnostic build saves the first VCN reset with
outstanding timestamps before native reset rewrites the writeback. The snapshot includes the submitted, cached
and writeback stamps and eight commits per queue with their actual VMID, flags, IB addresses and lengths.
Later reports retain that snapshot across replay and engine reinitialization. They also show the latest live
submission when available.

The decoder section records eight completed task releases and sixteen recent `unmapVA` calls. Entries use
Mach absolute time. For a known video VMID, compare the fault page with the unmap address and byte count.
The driver reads the VM context's task before calling native `unmapVA`. A release start time links an
unmap to an enclosing `freeMappings` on the same thread for the same GPU task. Known nonzero video VMIDs
also produce records outside a task release. VMID zero requires a matching release and the former VMID
still needs independent evidence. An unmap end time records the native function's return. The GPU may
execute the page-table update later.

`debug.nred_vcn_submissions` also reports observed writeback updates in decode, encode and low-latency
encode order. The timestamp callback compares the separate hardware writeback word with the driver's
cached value and counts a changed stamp once. Several jobs can complete between callbacks. The runner
checks the upstream reset and skipped-replay counts because recovery can also write the timestamp word.
A diagnostic run requires `firstReset=0`, `unmapVA=1` and `writebackTrace=1`.

On the host, the original X6000 initializers and commits ran against the native HWLibs TTL methods with an
in-memory ring sink. An injected enqueue error reproduced the returned success stamp on all three native channels.
The repaired encode, low-latency encode and decode frames matched the native single-IB frame byte for byte for
both primary and auxiliary input. The C++ tests also checked empty submission, discarded replay, multiple IBs,
wraparound and stable return stamps after build and submit failures under ASan and UBSan.
The Python ABI check requires `unicorn` and the matching system kernel collection.

```sh
python3 tools/check_vcn_submission_abi.py /tmp/nred-vcn-frames
xcrun clang++ -std=c++20 -O1 -g -fsanitize=address,undefined -INootedRed \
    tools/test_vcn_submission.cpp -o /tmp/nred-vcn-test
/tmp/nred-vcn-test /tmp/nred-vcn-frames
```

`tools/test_swiftrdp_vcn.py` takes the candidate kext, the SwiftRDP XCTest bundle and `--output DIRECTORY`.
For a copied bundle, also pass `--swift-package PATH` to the SwiftRDP checkout with the matching Release
executable. On a boot with the candidate loaded, it checks SwiftRDP's serial 1080p/4K AVC420 pipeline for
both encoder and decoder writeback activity, then runs the full Release suite with SwiftPM's original
`--parallel --num-workers 1` process isolation. It checks the scheduled XCTest cases against xUnit and the
1080p test's terminal result against the verbose XCTest log. SwiftPM on this host omitted `XCTSkip` from xUnit.
The runner then measures five isolated 1080p encode/decode/close runs and one continuous encoding run.
Ordinary assertion failures remain in the final result while allowing the remaining measurements to run.
A reset, skipped replay, new diagnostic report, submission failure or timeout stops the run. The runner cleans up its
test session, including XCTest workers in separate process groups, and reports any worker still blocked.
When a test stalls, it samples its XCTest worker before cleanup. On an aborted workload it saves available
AMD and IOAccelerator kernel messages for that workload's time interval and boot UUID in `*.kernel.log`.
Acceptance requires passing tests, advancing encode and decode counters on each handoff, and activity on
`VCN0EncLLQ` in at least one handoff. The counters cover the whole device, so other video workloads should be idle.

On `9704C1C3` the first full-HD XCTest stopped the runner on an H.264 color assertion. Five further runs of the
same test each finished encode, decode and close. All six gray levels passed in each run. Each of the five runs
added three `VCN0EncLLQ` and three `VCN0Dec` bridge records with zero submission failures. The IORegistry
`Commands Completed` counters showed zero delta. SwiftRDP's separate serial AVC420 pipeline test passed at
1920×1080 and 3840×2160. No GPU restart, panic or watchdog timeout report appeared during these runs.

The hardware encoder rejected the sRGB transfer-function property with `kVTParameterErr`. SwiftRDP's SPS
contained transfer characteristic 2 (unspecified), while the full-HD XCTest expects 13 (sRGB). An independent
VideoToolbox client encoded and decoded 41 full-HD frames with keyframes at indices 0 and 40. It also produced
2 for sRGB and produced 1 when asked for BT.709. SwiftRDP's original continuous test requests a 64×64
hardware session. Its creation returned `kVTInvalidSessionErr` before the VCN submission counts changed.
These failures and the zero completion-counter deltas leave the original validation criteria unmet.

At 19:38:31.759 Shanghai time on November 25, 2026, the full Release run on `9704C1C3` reset channel 14
again. The 1080p gray-level XCTest was in `VTCompressionSessionInvalidate`. The pre-reset log showed both
software and hardware at `0x155`, waiting for `0x156`. The latter was a primary submission from the encoder
on VMID 8 with a 13-dword IB at `0x400010280`. The VMID-zero fence submissions for software stamp `0x156`
appeared after recovery began. The decoder had completed `0x79` on VMID 11. The MM hub reported a read fault
on VMID 11 at `0x40000c000`, client 29. The Renoir table `mmhub_client_ids_renoir` in Linux
`drivers/gpu/drm/amd/amdgpu/gmc_v9_0.c` identifies read client 29 as `VCNU`.

The native event log placed PID 6175 (`VTDecoderXPCService`) inside the last recorded task release.
Its return preceded encode submission `0x156` by 2.46 ms. The release record contained no address range.
Whether it unmapped the fault page remains open. Offline replay of the recorded decode and encode IB descriptors
produced byte-identical native queue frames. The diagnostic Release build `A09487DF` includes the local
`dec0a87` Managed/Volatile change. Its kext and dSYM UUIDs match. The ABI and sanitizer checks passed.
A CPU-only SwiftPM fixture exercised normal completion, ordinary failures, skipped tests, partial reports
and termination after simulated resets or timeouts. `python3 tools/test_swiftrdp_vcn_runner.py` runs the
portable fixture checks on a Mac with Xcode.

At 20:52:07 Shanghai time on November 25, 2026, the full SwiftRDP Release run on `A09487DF` stopped during
the 1080p test. The XCTest stack showed `VTCompressionSessionInvalidate`. The encoder service was in
`VAUveEncoder::destroySession`, waiting in `IOAccelResourceFinishEvent`. Channel 14 had submitted `0xc3`
on VMID 7. The first reset captured both its cached and hardware completion values at `0xc2`.
The MM hub recorded a read fault at `0x40000c000` on VMID 8. An 8 KiB `unmapVA` at the same address and
VMID returned 3.986 ms before submission `0xc3`. The fault register gives no timestamp for the read.
On this build, only VMID-zero unmaps record an enclosing task release. The owner of the nonzero-VMID
unmap and the operation that issued the read remain unresolved.

The runner stopped before the isolated rounds. Two WindowServer watchdog reports belong to the validation
boot. The subsequent panic reported 120 seconds without a WindowServer check-in. An earlier panic written
at 20:46:58 reported a watchdog timeout during shutdown of the preceding boot. The hardware video
validation failed. Both this run and the preceding full run also produced the same three Swift array-bounds
traps in the GFX pipeline tests before reaching the 1080p case.

The native report for the 20:52 incident places the last decoder hardware timestamp update at
338.959838 seconds since boot, with `0x2a` submitted and completed. The 8 KiB unmap began about
0.39 ms later. The encoder submitted `0xc3` another 3.986 ms after the unmap returned. The writeback
records completion of the decoder submission. The report does not identify which command accessed
the fault page or when the MM hub latched the fault.

After the 20:54:43 restart, separate 720p H.264 hardware controls on the same driver completed.
In a single-frame decode, the final command referenced `0x40000c000` as its message buffer.
The decoder service released those 8 KiB with `0x14` submitted and completed. In a two-frame decode,
the same address was unmapped with `0x11` submitted and `0x10` completed. That session's final
command referenced a *different* message buffer, `0x400466000`, which was unmapped with `0x11`
completed. In both controls the releasing VM context was created by `VTDecoderXPCService` and its
task's cached PID was still `-1` at unmap. A two-frame encode also completed. Its 13-dword create
and close commands had different operation codes. Address, outstanding queue depth and IB length
alone therefore cannot identify an early release in the failed run. The available historical boot
logs also truncate the boot arguments, including on a confirmed UMA-enabled boot. They leave the
UMA-off comparison for this failure open.

SwiftRDP's serial native AVC420 test on that boot completed 1080p and 4K in 9.604 s using the same
Release XCTest binary as the failed run. All 28 low-latency encode submissions and all six decode
submissions in the trace had a subsequent matching hardware writeback on their own queue. No new
diagnostic report appeared. There was other video activity on this boot outside the measured interval.

| Session | Final decode writeback to message unmap entry | Message unmap entry to encoder close submission |
| --- | --- | --- |
| SwiftRDP 1080p | 0.440 ms | 4.419 ms |
| SwiftRDP 4K | 0.207 ms | 6.665 ms |

In both passing sessions the final decode command referenced its 8 KiB message at `0x40000c000`.
In the separate 720p single-frame control, that message had native resource type `0`, flags `0x103`,
and no `setFlags(2)` call from allocation through release. The encoder and decoder had distinct
128 KiB session buffers at `0x400040000` in their respective address spaces. Both used pooled UMA
resource type `0x40`, flags `0x3103`, and received `setFlags(2)`. The 13-dword encode commands had
opcodes `0x01000001` (create) and `0x01000002` (close) after relocation. These controls distinguish
the message from the session storage. The failed run still lacks a relocated command identifying
the reader of the faulting page.

On the November 25 22:23:15 Shanghai boot, kext `180FFCDE` (1.3.4, based on `3190816`)
passed all seven UMA workloads. The Managed buffer and texture each released 16 MiB of graphics
footprint while volatile. GPU refill and readback passed after discard. The isolated SwiftRDP
1080p/4K control completed in 10.703 s with 28 low-latency encode submissions and writeback
updates, and six decode submissions and updates. The full Release run finished 856 XCTest cases
with 71 failures and 66 passing Swift Testing tests. The 1080p XCTest finished in 0.641 s with
the existing SPS transfer-characteristic assertion. Three GFX pipeline XCTest processes trapped.

The first subsequent isolated 1080p round hung in `VTCompressionSessionInvalidate`. At
22:31:22.497 the kernel reported channel 14 waiting for stamp `0xfa`. The encoder service was
in `VAUveEncoder::destroySession` waiting in `IOAccelResourceFinishEvent`. Direct hardware
samples showed `0xf9` in the channel writeback word with `0xfa` submitted. The decoder had
completed `0x33`. The kernel reported an MM-hub read protection fault at `0x40000c000` on
VMID 11. The low-latency encoder used VMID 10.

The relocated command trace identifies the final decoder message at `0x40000c000` in the
VM context belonging to `VTDecoderXPCService` PID 6086. It was a native type `0` resource
with flags `0x103` and size 8 KiB. Native `unmapVA` began about 0.124 ms after the last
decoder timestamp update and returned 4.091 ms before the encoder close submission.
The driver's same-thread, same-task release marker was zero for this unmap. The relocated
encoder close referenced its own session at `0x400040000`. These observations identify
the resource and its owner. The fault register still gives no time for the read. At abort,
the counters showed 51 decoder submissions and 51 writeback updates, 250 low-latency encoder
submissions and 249 updates, one upstream reset and zero skipped replays. The remaining
four handoff rounds and the continuous case did not run. The kernel logged
`GPURestartEnd` for channel 14 at 22:31:33.507. The same WindowServer process was still
running twelve minutes later. This build fails video acceptance.

The same boot's trace covers both executions of the gray-level XCTest. In both,
the decoder used VMID 11 and the final command referenced the same 8 KiB message
address. At 22:29:03 the message unmap returned 8.928 ms before relocation of the
encoder close command and the test finished. At 22:31:17 that interval was
4.081 ms and the close stalled. In both runs the decoder VM context entered
`free` before encoder close relocation, by 2.105 and 2.317 ms respectively.
These intervals measure CPU-side events. The time of the GPU fault is unknown.

The next boot ran the same `180FFCDE` kext and Release XCTest binary with
`-NRedUnifiedMemory` removed. It repeated the UMA workloads, the native 1080p/4K
control, the full suite and then the isolated cases in the original order. The
1080p/4K control passed in 9.772 s. Both full runs completed 856 XCTest cases and
66 passing Swift Testing tests. All 70 cases that failed with UMA off also failed
with it on. The one additional failure with UMA on was
`MobileDesktopGeometryTests.testColorChangesDoNotReconfigureTheCaptureTarget`.

| Check on `180FFCDE` | UMA off | UMA restored at 22:54:02 Shanghai time |
| --- | --- | --- |
| Five isolated 1080p gray-level sessions after the full suite | All five closed in 0.508–0.525 s. Each had three decoder and three low-latency encoder writeback updates. Each failed only the SPS transfer-characteristic assertion (2, expected 13). | Not repeated |
| Original 64×64 explicit-IDR test | Session creation returned `-12903` with zero submissions or writeback updates on all three queues. | Not repeated |
| Additional 1080p encode/decode/close control | 41 frames decoded in a 0.996 s test, with keyframes at 0 and 40. | Same 41 frames and keyframes in a 1.335 s test. |
| VCN reset / skipped replay counters at the end of testing | 0 / 0 | 0 / 0 |

For the additional control, both boots recorded 43 submissions and 43 hardware
writeback updates on each of the decoder and low-latency encoder queues. The
restored boot ran only that workload. A repeat at 06:43 Shanghai time on
November 26 decoded all 41 frames in a 1.184 s test, with the same queue deltas,
keyframes, zero resets and zero skipped replays. The five completed closes with UMA off and
the stall with UMA on warrant checking how UMA places video-session storage.
The 8 KiB decoder message uses native placement in the recorded failure.
In the earlier 720p control, the two 128 KiB session allocations each belonged
to the same `IOAccelShared2` object as their respective video context. The
encoder process also allocated textures through a different `IOAccelShared2`
object. Process identity alone cannot distinguish those allocations. The cause
of the stalled close and the effect on throughput remain open.

On November 26 a 41-frame 1080p control on `180FFCDE` identified one 131,072-byte
allocation per video context. Each used copy flags `0x10002000`, had a backing
copy, and had no surface layout. Each context's share was connected before the
allocation. The encoder's 16 other 32 KiB video textures and the decoder's two
larger video textures used the same copy flags. Both 128 KiB mappings resolved to
`AMDAccelSysMemory` with flags `0x3103` on all 43 command observations per queue.

On November 26, the `1E1A4D05` boot placed the 128 KiB sessions in native
`AMDAccelVidMemory` with flags `0x5906` in all 274 encoder and 42 decoder command
observations. The seven UMA workloads and the 9.799 s 1080p/4K control passed.
The full suite finished 856 XCTest cases. Its 70 failing cases matched the
UMA-off run exactly, and all 66 Swift Testing cases passed. The 1080p gray-level
case finished in 0.514 s inside the full suite with the existing SPS assertion.
In the native control, median frame preparation plus encoding took 60.98 ms
at 1080p and 146.65 ms at 4K. The same test on `180FFCDE` with UMA off measured
60.51 ms and 146.39 ms. Each figure comes from one run per boot.

At 07:29:06 Shanghai time, the first subsequent gray-level session stalled in
`VTCompressionSessionInvalidate`. Channel 14 had submitted 250 and completed
249 on encoder VMID 8. The decoder had submitted and completed 51 on VMID 10.
Its 8 KiB message unmap returned 3.157 ms before the encoder close submission.
The unmap began about 0.523 ms after the last decoder timestamp update. The
MM hub reported a read fault at that message address on decoder VMID 10.
The report gives no timestamp for the hardware read. The runner stopped at this
reset before completing any of the five handoffs or the final 41-frame control.
At 07:30:01 the decoder also timed out during recovery. Its report preserved
the first reset's encoder writeback of 249. Moving only the 128 KiB sessions
therefore left the shutdown fault reproducible.

#### Native video working textures on November 26, 2026

Release `E635F13C` also sends the video context's linear working textures to the
native allocator. It checks copy flags `0x10002000`, absence of a surface layout
and the connected `IOAccelShared2` of an attached `AMDNavi10VideoContext` on the
owning accelerator. `debug.nred_uma.native_vcn_textures` counts matching placement
decisions. `tools/test_swiftrdp_vcn.py --verify-video-placement` checks that the
counter advances during each hardware workload. Allocation and command traces
independently showed `AMDAccelVidMemory` in all 754 encoder and 39 decoder backing
observations and all 751 encoder and 239 decoder command observations. These
included the 32 KiB encoder textures, both 128 KiB sessions and working textures
as large as 37,326,848 bytes.

On the 07:46:46 Shanghai boot, all seven UMA workloads passed. After the full
SwiftRDP Release suite, five consecutive 1080p gray-level sessions closed in
0.551–0.633 s. Each added three submissions and three hardware writeback updates
on both the decoder and low-latency encoder. Each reported only the existing
SPS transfer-characteristic assertion (2, expected 13). A separate 1080p control
decoded all 41 frames, with keyframes at 0 and 40, and added 43 submissions and
43 writeback updates on each queue. The boot ended validation with 106/106
decoder and 305/305 low-latency encoder submissions/writebacks, zero resets,
zero skipped replays and no new GPU diagnostic report. The close that stalled
on `1E1A4D05` completed in all five rounds on this boot. The trace cannot identify
the individual texture or the precise hardware access that caused the prior fault.

The full suite ran 856 XCTest cases. Its 71 failing cases matched the earlier
UMA-enabled `180FFCDE` run exactly. The UMA-off run and `1E1A4D05` each skipped
the physical-capture color-change case because the test found no physical source,
and had 70 failing cases. All 66 Swift Testing cases passed on each boot. The
original 64×64 IDR control still returned `-12903` before submitting any work.

| Median frame preparation plus native AVC420 encoding | `180FFCDE`, UMA off | `1E1A4D05`, UMA on | `E635F13C`, UMA on |
| --- | ---: | ---: | ---: |
| 1920×1080 | 60.51 ms | 60.98 ms | 62.95 ms |
| 3840×2160 | 146.39 ms | 146.65 ms | 146.28 ms |

Each column represents one run of the same Release test binary. These timings
cover server-side preparation and encoding. Client display latency and sustained
throughput still need separate measurements.

### Flag off vs on (2026-11-24)

Kext `B7DC1F8C` (this branch), two boots per round that differ only in `-NRedUnifiedMemory`. No GPU reset in any
boot. The external display was offline: round 1 (the three workloads above and the churn tool) ran on the fallback
virtual display, round 2 (Geekbench 7 and the desktop animation) on a 3840x2160 60 Hz virtual display.

The historical working-set shader committed a result from just one lane per texture. Its rates are nominal
payload GiB divided by elapsed seconds, and the holder pass times describe that same workload. Full-payload
measurements require the lane-checked probe. Churn timings throughout the November 23–24 results used the
original 30-iteration write-only probe.

| Workload | Flag off | Flag on |
| --- | --- | --- |
| Metal recommended working set | 512 MiB | 16,896 MiB |
| Legacy working-set rate, 256 MiB (nominal GiB/s) | 38.0 | 35.3 |
| Legacy rate, 512 MiB / 1 GiB / 2 GiB / 3 GiB (nominal GiB/s) | 8.6 / 6.9 / 6.0 / 5.9 | 35.8 / 36.1 / 38.0 / 38.1 |
| 4K render, 0.5 / 1.3 / 2.3 GiB | 19.4 / 5.27 / 3.62 fps | 19.6 / 7.41 / 4.95 fps |
| 4K export, 4 layers x 120 frames / 10 layers x 60 frames | 30.2 / 70.4 s | 11.3 / 28.4 s |
| Hardware HEVC decode of the export | 98 / 115 fps | 98 / 115 fps |
| Geekbench 7 GPU, Metal / OpenCL | 10,960 / 1,229 | 13,767 / 1,849 |
| Animation, frames above 25 ms without scrolling: idle | 17.7% | 1.8% |
| ... with 1 GiB / 2 GiB held by another process | 19.6% / 14.8% | 1.2% / 1.9% |
| ... full-screen exit / enter, idle | 46.7% / 27.0% | 1.1% / 6.8% |
| Holder pass over 1 GiB / 2 GiB | 171 / 356 ms | 29 / 56 ms |
| 64 MiB / 16 MiB texture create, first use, release | 2.19 / 0.79 ms | 7.95 / 2.20 ms |

With the flag off, the part above the carve-out goes through GART and the driver pages about 2 GB of surfaces and
textures during round 1. At 256 MiB the legacy probe reported a 7% lower rate with the flag on. On the
virtual display a frame is a WindowServer command-buffer submission interval; scrolling scenes are left out because
the virtual display produces too few submissions in them. The Geekbench results are
[433454](https://browser.geekbench.com/v7/gpu/433454), [433470](https://browser.geekbench.com/v7/gpu/433470) (off)
and [433577](https://browser.geekbench.com/v7/gpu/433577), [433588](https://browser.geekbench.com/v7/gpu/433588) (on).

### This branch (2026-11-23)

Kext `FE9FAF85`, built from the pool and coalescing commit of this branch, in two boots: one without
`-NRedUnifiedMemory`, one with it. In the second boot the pool and coalescing were switched off for the middle phase
with runtime switches that a later commit removed. All runs start a few minutes after login, and such runs differ by
about 5 points between boots.

| | flag off | on | on, pool and coalescing off |
| --- | --- | --- | --- |
| 64 MiB texture create, first use, release (median) | 2.52 ms | 8.14 / 8.12 ms | 33.0 ms |
| 16 MiB texture create, first use, release (median) | 0.85 ms | 2.18 / 2.16 ms | 8.27 ms |
| Frames above 25 ms, full-screen enter | 76.8% | 12.9 / 15.3% | 46.3% |
| Frames above 25 ms, full-screen exit | 71.7% | 4.8 / 4.2% | 6.2% |
| Frames above 25 ms, Mission Control enter | 51.6% | 12.5 / 9.1% | 11.3% |
| Frames above 25 ms, Mission Control exit | 57.1% | 6.7 / 24.4% | 47.2% |
| Frames above 25 ms, all scenes without scrolling | 27.8% | 6.4 / 7.5% | 13.7% |

- Without the flag, the `debug.nred_uma` sysctls are absent, the Metal and OpenGL baseline readbacks pass, and the
  GPU was not reset. Creating a texture is faster in the carve-out because nothing has to be wired, but the
  animations run into the full carve-out.
- With the flag, the pool, hand-back and coalescing were on and all probes passed, including the 736-group pressure
  case. The
  cross-process check found the marker in 0 of 8 textures while the same-process control saw reuse in 4 of 4.
  `pool_return_skips` and `pte_fallbacks` stayed at 0. Accelerator wired memory (live resources, without idle pool
  descriptors) was 438 MB after 60 s idle and 449 MB after the probes.

The build without the switches (kext `4D2DE009`) was checked in one more boot with the flag on:
64 MiB create to release 7.94 ms, 16 MiB 2.23 ms, all probes passed including the pressure case, and the
cross-process check found 0 of 8 markers with 4 of 4 in the control. In that boot 2727 backings were handed back
and one was skipped: `returnPooledDescriptors` declined it (purgeable state, an extra reference or an attached
resource), and the backing was freed the native way instead.

The build that picks the buckets by RAM (kext `84A993E8`) was checked in two more boots, both with
the flag on. The external display had gone offline the evening before, after display sleep, and was not detected in
these boots, so WindowServer ran on a virtual 1280x1024 display. The probes are valid; the animation runs recorded
no display transactions and are not reported.

| | `nred-uma-wired-mib=4095` (native buckets) | default (large buckets) |
| --- | --- | --- |
| Requests above 24 MiB served by the pool | 0 | 54 |
| 64 MiB texture create, first use, release (median) | 21.2 ms (pageable path) | 7.95 ms |
| 16 MiB texture create, first use, release (median) | 2.15 ms | 2.07 ms |
| Backings handed back during churn and animation | 1375 | 1250 |
| Cross-process check (control) | 0 of 8 at 16 MiB (4 of 4) | 0 of 8 at 64 MiB (4 of 4) |
| Pool wired after the probes | about 75 MiB | about 200 MiB |

All probes passed in both boots: readback with surfaces, cross-process IOSurface and layouts, purgeable states,
Managed resources, OpenGL smoke, VBO and full cases, a 5 GiB reclaim run against the 4 GiB budget, and the 23 GiB
pressure case with the default budget. `pool_return_skips` and `pte_fallbacks` stayed at 0.

The build that limits PTE coalescing to unified memory backings and publishes the registry state (kext `EE30E22B`)
was checked in one boot with default boot arguments. The registry showed `active`, `large`, and
hand-back and coalescing on. 64 MiB create to release took 7.89 ms and 16 MiB 2.08 ms, about 178,000 coalesced
packets were written, and the cross-process check found 0 of 8 markers (control 4 of 4). The managed, OpenGL and
23 GiB pressure probes passed. The purgeable probe failed once while the external display came back online during
the run, which made WindowServer release and re-create surfaces and kept the device-wide `bytes_live` growth at
500 MiB; it passed three times when run again in the same boot.

The final build (kext `B7DC1F8C`: the cleanup, the `DeviceProfile` gate and the budget sites applied through
`UserPatches`) was checked in one boot with default boot arguments and the 4K display on. The registry showed
`active`, `large`, hand-back and coalescing on, and `NRedCompatUnifiedMemory` = 1. Both Metal sites and both OpenGL
sites were applied through `UserPatches` (`metal_budget_pages` 2, `gl_budget_sites` 3). 64 MiB create to release
took 8.19 ms and 16 MiB 2.25 ms, the cross-process check found 0 of 8 markers (control 4 of 4), all probes passed,
and `pool_return_skips` and `pte_fallbacks` stayed at 0. Frames above 25 ms: full-screen enter 11.3%, full-screen
exit 3.8%, Mission Control enter 0% and exit 4.3%, all scenes without scrolling 4.3%.

### Pool, hand-back and PTE coalescing (2026-11-23)

Kext `5DAA2EEC`, one boot, switching the pool, hand-back and PTE coalescing on and off with development switches
that this branch no longer has.

- 64 MiB private texture create, first use and release, median: all off 33.5/35.9 ms, pool only 18 ms,
  coalescing only 24 ms, all on 8.2/8.4 ms. 16 MiB: 8.2/8.7, 5.5, 5.6 and 2.1/2.2 ms.
- Frame intervals above 25 ms on the 10-bit 4K desktop: full-screen enter 40.3/39.2% to 13.9/15.3%, Mission Control
  exit 63/57% to 33/41%, all scenes without scrolling 19.2/18.6% to 12.7/12.8%. Mission Control enter stays at
  41-46% in every configuration; the bottleneck is GPU queueing.
- About 1930 backings are returned per run. `pool_return_skips` and `pte_fallbacks` stay at 0. WindowServer's wire
  time for large textures fell from about 1.2 s per run to tens of milliseconds. About 10% of the descriptors for
  requests above 24 MiB had to be created.
- Isolation: one process writes 8 marked 64 MiB private textures and exits, then another process creates textures of
  the same size and reads them. No marker was found. A control in the same process saw reused contents (2-4 of 4),
  which shows the check can see reuse.
- All probes passed, including the 23 GiB pressure case. Dirty descriptors drained after 60 s idle, and accelerator
  wired memory fell to 436 MB after the pressure.

### Against the carve-out

In the same boot, with the measured process's resources forced onto the native carve-out: buffer read bandwidth
was 97% of the carve-out, texture reads and writes 94-99%, and ALU throughput the same. Create to release of a
16/32/64 MiB texture took 2.3/4.7/8.0 ms against 0.85/1.35/2.75 ms. The carve-out runs filled about 500 MiB of VRAM
and hit page-offs and reclaim waits. With unified memory a 1 GiB buffer reads at 36 GB/s.

Dependent-load latency is 17-22% higher because of address translation. Unified memory PTEs are system, snooped
entries (`0x77`) with fragment 0 in most cases, so each covers 4 KiB. Carve-out PTEs (`0x71`) use fragments 1-4,
up to 64 KiB. Walking 64 MiB in page order (a DRAM miss per load and about one TLB miss per 64 loads) shows no
difference (0.98). Moving to a new 4 KiB page on every load is 41% slower, and 64 KiB hops are 47% slower. A 64 MiB
private buffer copy is about 16% slower, 5-8% on each of the read and write sides. Each 64 KiB fragment requires
sixteen consecutive physical pages with matching alignment. Reclaim can release them after the PTEs are retired.
The optional 64 KiB path checks physical layout each time it rewires a pageable backing.

### Earlier object-level checks (2026-11-21)

Kext `F7C4DF32`, before the pool and mapping reuse:

- A 23 GiB Metal working set at the native residency limit, while 816 MiB of OpenGL textures, 576 MiB of VBOs and a
  cross-process IOSurface were kept. 736 private buffers, 736 render targets, 15 GL textures, 9 VBOs and three
  mappings of the same IOSurface each went through PTE removal, unwire, rewire and commit at the original task and
  GPU VA. All readbacks passed and no allocation failed.
- The nine VBOs' imported client descriptors were wired, unwired and rewired for later uploads, and passed readback
  and CPU updates after pressure.
- Eight Managed buffers and eight Managed textures came back to their original task and GPU VA after pressure.
- 20 Metal processes mapped the same GPU VA in their own page tables and read back different contents. Both the GFX
  and MM VM hubs reused VMIDs across tasks, and the native driver issued the invalidation before each new
  allocation completed.
- After a 4K resolution change and 96 full-screen Metal drawables, WindowServer's offscreen primaries used system
  memory and all 775 recorded scanout bindings used VRAM. 4K HEVC playback worked. Resources created before a
  display power drop from state 4 to 1 read back correctly after it returned to 4.
- A displayable 4K IOSurface used VRAM, and an offscreen IOSurface of the same size used pageable system memory.
- OpenGL 4x MSAA and depth/stencil primaries with separate auxiliary descriptors used VRAM. A 2048x2048 Metal color
  texture with 64 KiB of inline DCC metadata was paged as a whole. `GL_STORAGE_CACHED_APPLE` and
  `GL_STORAGE_SHARED_APPLE` went through the native client descriptor and a pageable primary.
- Hot-plug of the 4K DisplayPort display: after reconnecting, Managed resources and the cross-process IOSurface read
  back correctly and WindowServer kept running. The panel stayed dark during that test because its DDC brightness
  had been set to 0. A hot-plug starting from a visible desktop has not been repeated.

## Limitations and open items

- Hardware validation is outstanding for system sleep and wake with live resources, hot-plug from a visible
  desktop, recovery of the review build after a GPU reset, and days of mixed use.
- Only `25G332` is supported. Every other build turns the feature off.
- Other Renoir-family device IDs take the same code path but have not been tested. Raven, Raven2 and Picasso are
  excluded.
- The new pressure targets and byte limits passed host policy tests. Actual wired memory released, refill latency
  after pressure subsides, committed texture cache hits and encoded 64 KiB coverage still need measurements on the
  final driver. The bucket choice on machines below 16 GB also awaits hardware coverage.
- The probes and the measurements come from one machine with a 512 MiB carve-out.
- 4 KiB PTEs cost 17-22% in dependent-load latency compared with the carve-out.
- Mission Control enter differs between boots (41-46% long frames in the 2026-11-23 afternoon boot, 9-13% in the
  boot of this branch) and does not depend on the pool or coalescing. Earlier traces placed it in GPU queueing.
- The 10-bit half-float composition load is not a memory management problem. With an 8-bit framebuffer, long
  frames in full-screen enter fall from 47% to 10% on the same machine.
