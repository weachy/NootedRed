# X5000 Unified Memory (Renoir)

With `-NRedUnifiedMemory`, the Renoir accelerator places textures, IOSurface device caches and data buffers in
pageable system memory instead of the firmware carve-out (512 MiB on the test machine). Metal, OpenGL and
WindowServer keep using the normal system APIs. The code is in `NootedRed/X5000Memory.cpp`, `X5000Memory.hpp` and
`X5000MemoryPolicy.hpp`. The probes and the policy test are in `tools/`.

The feature is off by default. It installs only on Renoir-family devices, and only when the loaded
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

DCC metadata that the Metal driver places inside a normal texture allocation, and reaches through the same GPU VA,
pages together with the texture.

OpenGL linear vertex buffers can also use a pageable primary allocation. The driver imports a native system memory
descriptor through `newResourceWithClientBuffer`. The policy checks that the resource has no texture format or
layout, comes from a client buffer, belongs to the same GPU task, and that the task has a per-task page table. The
native upload and synchronization code then writes the data into the unified memory primary.

With unified memory on, special allocations keep the native VRAM allocator and the extended `allocateLargeBlocks`
is not installed. The existing OpenGL low-VRAM budget fix stays as the fallback.

## Residency and reclaim

When the residency budget is reached, the native LRU reclaims only system memory that has no pending prepare
reference and whose GPU events have completed. `IOAccelSysMemory::unwire` removes the PTEs, keeps the mapping object
and the GPU VA, and unwires the pages. A later submission maps the same backing back to the same GPU VA.
Resource-level `pageoff` involves backing migration and blits and is tested separately.

For purgeable requests the native resource methods first handle the retained state and the cache valid bits.

- `Empty` asks for an immediate discard. When the backing has no prepare reference and all GPU events have
  completed, unified memory calls the native `unwire`, which removes the PTEs and releases the pages
  (`purge_unwires`).
- `Volatile` only allows a discard under memory pressure. The backing stays wired and the native LRU reclaims it
  when the residency budget requires it. WindowServer marks surfaces `Volatile` during window and full-screen
  transitions and often uses them again right away, so unwiring them at once would cause a rewire.

In `25G332` the hook sits between `lock_busy` and `unlock_busy` in `set_resource_purgeable`. The native non-blocking
LRU path also checks the prepare count and the GPU events before `unwire`.

## Budgets

Metal clients get three quarters of the wired limit as the recommended working set, and half of that as the command
segmentation threshold, so several command buffers and the system graphics clients share the headroom. OpenGL gets
its command resource list budget, half of the recommended working set, through the same device config interface.
Both budgets are published only for the Renoir device with unified memory on. The physical carve-out size is
reported separately. An OpenGL device that does not receive a budget keeps the existing compatibility policy.

## Page tables

Page tables stay in the carve-out. `AMDHWVMM` allocates them and SDMA updates them. X5000 writes them with `PTE_PDE`
packets, one packet per physically contiguous range. Pageable system memory is scattered page by page, so each page
needs its own packet.

A CPU-written leaf page table was tried and dropped. The asynchronous clear in `AMDHWVMContext::clearWithDMA`
overwrites CPU writes that finish earlier, so every leaf write would have to come from the CPU. The GPU does not
snoop the CPU cache for page tables, so write-back leaf pages need a flush per cache line. With the flushes the
barrier wait fell to 0.06 ms, but WindowServer kernel CPU time per run rose from 6.2 s to 11.7 s and long frames in
the light animation from 16% to 34%. A write-combined mapping caused GPU page protection faults. Linux amdgpu also
updates the graphics page tables of Renoir with SDMA.

Runs of single-page PTE updates are coalesced. Inside `AMDHWVMContext::mapVA`, single-entry updates of consecutive
PTEs from `mapVMPT` are staged and written as one SDMA `WRITE_LINEAR` packet with two dwords per entry, as in
amdgpu's `sdma_v4_0_vm_write_pte`. The thread flushes its staged entries before any other reservation or commit on
the same command pool and when the outermost `mapVMPT` returns, so the pool holds every packet when `mapVA` ends.
If a large reservation fails, the entries go to the native single-entry writer. SDMA stays the only page table
writer, and `unmapVA` keeps the native path. A 64 MiB texture needs about 190,000 dwords of PTE packets. The GPU job
wait on the page table barrier drops from about 11 ms to 2-3 ms. Counters: `pte_batches`, `pte_batched_entries`,
`pte_fallbacks`.

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

- When only orphans reference a backing, the native code calls `IOAccelSysMemory::orphanIt`. For a pooled backing
  this returns the descriptors to the pool while orphan PTEs may still be valid. Unified memory first releases the
  orphans whose GPU events have completed, as `IOAccelMemory::release` does. If all of them go, the backing is
  destroyed instead of orphaned. Busy maps stay orphaned, as in the native code. The same task can then take the
  descriptors back and inherits the merged GPU event. That event covers the old GPU work. The ordering against the
  clear of the old PTEs and the TLB invalidation has not been measured.
- Orphan lookup matches the task only, not the requested GPU address. The native code clears the flag after it
  creates a fixed-address map, but an existing orphan could still be revived. Unified memory releases the idle
  orphans of the backing before a fixed-address request. Unified memory primaries never hold fixed-address
  resources.

On 2026-11-22 (kext `8C62ED5A`), compared with the unified memory build without mapping reuse: PTE commits per
animation run fell from 4741 to 1686, WindowServer page table command buffers from 33758 to 4519 (median length 4080
to 602 dwords), and the median scheduler hold of graphics submissions in the light animation from 6.47 ms to
0.01 ms (p90 18.5 ms to 0.50 ms).

## Native buffer pool

For GPUs that use system memory as primary storage, `IOAcceleratorFamily2` provides
`IOAccelBufferMemoryDescriptorPool2`. `IOGraphicsAccelerator2::start` creates it only when bit 21 of the driver
config dword at `0xc90` is set, and the AMD driver does not set it. Unified memory creates the pool for the Renoir
accelerator at the same check without changing the config bit, so the other native behavior tied to that bit stays
unchanged.

### Buckets

The pool keeps wired, pageable, purgeable descriptors in five buckets. The pool timer refills each bucket to 24
descriptors (a constant in `initWithAccelerator`), and a request is capped at 24 times the first bucket size. The
native first bucket is 1 MiB, so the cap is 24 MiB. WindowServer's 10-bit full-screen half-float textures are
25-64 MiB.

Before the pool is created, unified memory rewrites `sWiredPoolInfo` and `sPurgeablePoolInfo` to buckets of 4 MiB,
1 MiB, 256 KiB, 64 KiB and 4 KiB, which raises the cap to 96 MiB. A request gathers at most 64 descriptors in a stack
array that the native code does not bounds-check. With the new buckets a request up to 96 MiB needs at most 47. The
rewrite first verifies the table loads, the prefill constant, the cap check and the stack array size in
`initWithAccelerator` and `newWiredDescriptorsWithLength`. On any mismatch the native buckets stay, and a later
failed check writes the original tables back. Larger requests, or requests the pool cannot serve, take the existing
pageable path. System memory allocations outside unified memory do not use the pool. Refill and zeroing run on the
native timer under the accelerator lock, which checks for waiters after each descriptor.

### Hand-back

Natively, descriptors return to the pool only from `IOAccelSysMemory::orphanIt`. Pooled backings almost always end
in `IOAccelMemory::release` instead: once the mapping events complete, it calls `pruneOrphanedMappings` and frees
the backing. Unified memory takes over that call site (`release+0xa6`) and its own `orphanIt` route:

- it adds a wire reference before the prune, so the last `release_pte` does not unwire the whole descriptor set;
- it collects the page table update event of the `AMDHWVMContext` whose mappings were released (`context+0x40`, the
  SDMA stamp that covers the PTE clear and the TLB invalidation);
- when the backing is destroyed, it calls `releaseDescriptors` as `orphanIt` does and returns the descriptors with
  that event.

The same task can reuse these dirty descriptors directly and inherits the event. Another task gets them only after
the native timer has waited for the event and zeroed them. When a task's `IOAccelShared2` is released, unified
memory re-keys that task's dirty descriptors to `kernel_task`. They are then zeroed before any reuse, so a new task
that gets the same task address cannot receive old contents. Each backing is returned once.

An `Empty` request on a pooled backing also unwires at once. The native code drops emptied descriptors when they
return to the pool.

### Footprint

`releaseDescriptors` keeps a returned descriptor while its bucket holds fewer than its wired capacity plus its
purgeable capacity. The capacities are the native ones, and the new 4 MiB bucket gets 32 wired and 32 purgeable.
The pool can therefore keep about 400 MiB. On the test machine after a day of use, `pool_state` read
`29/32/0 64/52/0 32/6/0 32/0/0 22/12/0` (wired/purgeable/dirty per bucket): about 190 MiB wired and 180 MiB
purgeable.

Only the purgeable part can be reclaimed by the VM. The timer refills buckets up to 24 but does not trim wired
descriptors above that, and accelerator wired memory stayed at 436-455 MB after the 23 GiB pressure probe. That is
0.3% of 64 GiB, and would be about 2.3% of an 8 GiB machine. A RAM-based cap is not implemented yet.

### Effect

Before the pool, each window cache create or destroy allocated pageable memory, faulted and zeroed it page by page,
wired it, and unwired it on release. In a kernel stack sample on 2026-11-22, wire and unwire took 15.5% and 13.5% of
WindowServer kernel CPU time. Wiring a new large texture happens under the accelerator lock. In a 64 MiB and 16 MiB
create/release loop, WindowServer was blocked on that lock for 151 ms in total (longest 26.6 ms). With the pool and
hand-back it was 17 ms (longest 3.1 ms).

Counters: `pool_allocations` (backings served by the pool), `pool_returns`, `pool_return_pieces`,
`pool_return_skips`, `pool_large_requests` (requests above 24 MiB), `pool_rekeyed` and `pool_state`.

`pageon` keeps the driver's native VRAM staging when it needs a temporary copy. A pageable staging route was
removed, because the native staging branch was never reached during the animation benchmark and all probes.

## Configuration

Add `-NRedUnifiedMemory` to the boot arguments. Lilu must load before NootedRed. The large pool buckets, hand-back
and PTE coalescing are on by default with it. To roll back, remove the argument.

- `debug.nred_uma.active` is `1` once the driver routes are installed. `debug.nred_uma.pool_state` reads `none`
  until the pool exists, and `debug.nred_uma.pool_large` reads `1` only when the large bucket tables are installed.
  NootedRed also logs `Creating the native buffer pool` and `Large buffer pool buckets installed`, but on the test
  machine these lines did not reach the unified log, so use the sysctls to check.
- Each part acts only after it has verified that its routes are in place. Otherwise it turns itself off and keeps
  the native behavior.
- These sysctls switch a part off at runtime (root). Reads report the effective state, and a reboot restores the
  defaults:
  - `debug.nred_uma.pool_large=0`: requests above 24 MiB take the pageable path;
  - `debug.nred_uma.pool_return=0`: stop returning descriptors to the pool;
  - `debug.nred_uma.pte_coalesce=0`: write PTEs one entry at a time.

  The bucket tables are set when the pool is created and do not change at runtime.
- `nred-uma-wired-mib=<MiB>` lowers the residency budget, for example `768` to trigger native reclaim in tests.

## Testing

Record for every run: the macOS build, the source commit and any local changes, the kext UUID from
`kmutil showloaded`, the boot arguments, and `sysctl debug.nred_uma` before and after.

Build the Metal probe and run a small readback from the repository root:

```sh
xcrun clang -fobjc-arc -O2 -Wall -Wextra -Werror \
  -framework Foundation -framework CoreGraphics -framework IOSurface -framework Metal \
  tools/x5000_uma_probe.m -o /tmp/x5000_uma_probe
codesign -s - /tmp/x5000_uma_probe
sysctl debug.nred_uma
/tmp/x5000_uma_probe --groups 1 --require-budget --require-surfaces --surface-ipc --layouts
```

- Capacity: with a 4K desktop active, `--groups 36 --require-budget`.
- Reclaim: boot with `nred-uma-wired-mib=768` and run
  `--groups 36 --idle 10 --require-budget --require-reclaim --require-surfaces --surface-ipc`. The probe keeps
  private buffers and render targets, checks that `bytes_live` grows by more than 512 MiB and verifies the GPU
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
- `--seed` sets the first check byte, so parallel processes can use different data to check task isolation at the
  same GPU VA.
- `--hotplug 300` keeps the resources while the main external display goes offline and comes back at its original
  resolution, then reads them back. Combine it with `--surface-ipc` to cover IOSurfaces mapped into two processes
  before the reconnect. Check on the panel that the picture was visible before unplugging and that the desktop
  comes back and updates.
- `--baseline` runs the readback without unified memory, for a boot without `-NRedUnifiedMemory`.

The driver counters are per device and include other processes on the same device.

The OpenGL probe samples five textures in one draw, which covers a command larger than the old resource list
budget and several resource groups. The full case keeps 816 MiB of textures on an active desktop with 272 MiB per
draw. It requires both budget hooks, the new device budget, a completed GPU readback and more than 512 MiB of
pageable primary backing. `--smoke` uses smaller textures.

```sh
xcrun clang -std=c17 -DGL_SILENCE_DEPRECATION -O2 -Wall -Wextra -Werror \
  -framework OpenGL tools/x5000_uma_gl_probe.c -o /tmp/x5000_uma_gl_probe
codesign -s - /tmp/x5000_uma_gl_probe
/tmp/x5000_uma_gl_probe --smoke
/tmp/x5000_uma_gl_probe --layouts
/tmp/x5000_uma_gl_probe --buffers
/tmp/x5000_uma_gl_probe
```

`--buffers` writes nine 64 MiB VBOs and reads back their first and last markers. With `--wait-for-pressure` the
probe keeps the resources and reads them again after an empty line on standard input. The VBO case then updates the
buffers from the CPU and checks that the GPU sees the update.

`python3 tools/test_x5000_memory_policy.py` checks the placement policy, the residency state transitions and the
budget calculation on the host.

## Results

Test machine: Renoir `0x1636`, 64 GiB RAM, 4K 10-bit DisplayPort display, macOS 26.7.2 (`25G332`).

### This branch (2026-11-23)

Kext `FE9FAF85`, built from this branch, in two boots: one without `-NRedUnifiedMemory`, one with it. In the second
boot the pool and coalescing were switched off at runtime for the middle phase. All runs start a few minutes after
login, and such runs differ by about 5 points between boots.

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
- With the flag, the defaults read `1 1 1` and all probes passed, including the 736-group pressure case. The
  cross-process check found the marker in 0 of 8 textures while the same-process control saw reuse in 4 of 4.
  `pool_return_skips` and `pte_fallbacks` stayed at 0. Accelerator wired memory was 438 MB after 60 s idle and
  449 MB after the probes.

### Pool, hand-back and PTE coalescing (2026-11-23)

Kext `5DAA2EEC`, one boot, switching `pool_large`/`pool_return` and `pte_coalesce` at runtime. The final build only
removed development switches.

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
private buffer copy is about 16% slower, 5-8% on each of the read and write sides. Closing this gap needs physically
contiguous backing aligned to at least 64 KiB, so that `mapVMPT` emits fragments, and such backing cannot be paged
out while the resource is alive.

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

- Not yet validated: system sleep and wake with live resources, hot-plug starting from a visible desktop, a real
  GPU reset, and days of mixed use.
- Only `25G332` is supported. Every other build turns the feature off.
- Other Renoir-family device IDs take the same code path but have not been tested. Raven, Raven2 and Picasso are
  excluded.
- The pool keeps about 190 MiB wired when idle and does not shrink under memory pressure (see Footprint).
- 4 KiB PTEs cost 17-22% in dependent-load latency compared with the carve-out.
- Mission Control enter differs between boots (41-46% long frames in the 2026-11-23 afternoon boot, 9-13% in the
  boot of this branch) and does not depend on the pool or coalescing. Earlier traces placed it in GPU queueing.
- The 10-bit half-float composition load is not a memory management problem. With an 8-bit framebuffer, long
  frames in full-screen enter fall from 47% to 10% on the same machine.
