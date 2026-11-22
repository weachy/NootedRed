# X5000 统一内存接入

## 应用如何用上

NootedRed 启动时接入 `IOAcceleratorFamily2` 的资源分配和 `AMDRadeonX5000` 的 GPU 映射。Metal、OpenGL 和 WindowServer 照常调用系统 API。符合条件的缓冲区、纹理和离屏表面由 `IOAccelSysMemory` 提供可分页系统内存 backing，X5000 为对应 GPU 任务保留 GPU VA，并在提交时准备 backing。原生离屏缓存和 IOSurface 设备缓存共用放置策略，该策略检查任务页表、资源布局和系统内存描述符。IOSurface 纹理别名跟随同一个缓存拥有者，沿用原生 CPU/GPU 同步。直接 scanout、外部设备布局和由内核创建独立辅助描述符的分配走硬件原生路径。Metal 驱动附在普通纹理分配内、通过同一 GPU VA 访问的 DCC 数据随纹理一起分页。

OpenGL 的线性顶点缓冲区也可以走可分页主分配。驱动通过 `newResourceWithClientBuffer` 导入原生系统内存描述符。放置策略核对资源无纹理格式和布局、来源是客户端缓冲区、GPU 任务归属匹配且已建立每任务页表。满足这些条件时，原生上传及同步逻辑把数据写入统一内存主分配。

达到驻留预算时，原生 LRU 只回收没有正在准备的引用且 GPU 事件已结束的系统内存。`IOAccelSysMemory::unwire` 撤掉 PTE，保留映射对象与 GPU VA，再解除对物理页的锁定。后续提交可以把同一 backing 重新映射到原 GPU VA。资源级 `pageoff` 涉及 backing 迁移和 blit，有独立的验收项。

应用同步请求 `Volatile` 或 `Empty` 时，原生资源方法先处理保留状态和缓存有效位。`Empty` 要求立即丢弃：没有 prepare 引用且 GPU 事件全部结束时，统一内存调用原生 `unwire` 撤掉 PTE 并释放物理页。`Volatile` 只表示允许在内存压力下丢弃，统一内存保留 wired backing，由原生 LRU 在驻留预算压力时回收，避免 WindowServer 在窗口/全屏过渡中刚标记 `Volatile` 又立即重新 `wire`。`purge_unwires` 记录由 `Empty` 请求触发的解除锁页次数。

本机 `25G332` 的 `IOAcceleratorFamily2`（UUID `6C4A16B1-66DC-3A8B-9F0B-D3E027B16EB1`）中，该钩子的调用点位于 `set_resource_purgeable` 的 `lock_busy` 和 `unlock_busy` 之间。原生 LRU 的非阻塞回收路径也在 `unwire` 前检查 prepare 计数和 GPU 事件完成状态。

Metal 客户端用 wired 上限的四分之三作为建议工作集，命令分段阈值取建议工作集的一半，供多个命令和系统图形客户端共享驻留余量。OpenGL 通过同一个设备配置接口取得命令资源列表的预算，按建议工作集的一半计算。两个预算只发布给已启用统一内存的目标 Renoir 设备，物理 carve-out 容量单独报告。OpenGL 设备没有收到预算时，资源列表使用原有兼容策略。

`mac-gfx-research/AppleIntelTGLGraphics_hlil.c` 中，`IGMemoryManager` 分别管理保留内存、全局 GTT 和页表节点池。`IGPagePool` 为 `IGHardwarePerProcessPageTable64` 供给页表页，`IGAccelSysMemory` 管理资源的系统内存 backing，`IGAccelMemoryMap` 分别分配 GPU VA 和提交页表。X5000 路径沿用这个分工，分别接入 macOS 的可分页描述符和 AMD 的每任务页表。Intel 驱动在提交映射时由 CPU 写系统内存中的页表，TLB 失效命令随同一条命令流发出。

### 页表

页表保留在原生 carve-out 中，由 `AMDHWVMM` 分配，并经 SDMA 更新。X5000 用 `PTE_PDE` 命令写页表项，一条命令覆盖一段物理连续的内存。可分页系统内存逐页分散，每页需要一条命令。WindowServer 不断删除并重建引用同一 IOSurface 缓存的资源。同一任务对同一 backing 只有一个 `IOAccelMemoryMap`，由引用它的资源共用，最后一个资源删除时映射随之释放，下一个资源再新建。`commit_pte` 和 `release_pte` 每次都写入或清除整个映射。2026 年 11 月 22 日的跟踪中，WindowServer 每秒向页表队列提交约 900 个写满的命令缓冲区。轻量动画期间，图形提交在调度器中停留的中位数为 6.5 ms，其中约一半在等页表屏障。关闭统一内存时，WindowServer 的 GPU 工作在提交后约 0.05 ms 开始执行。

同日试验过由 CPU 写系统内存中的叶页表。`AMDHWVMContext::clearWithDMA` 的异步清零会覆盖先完成的 CPU 写入，因此叶页的全部写入都要由 CPU 完成。GPU 读页表时不探查 CPU 缓存，写回缓存的叶页需要逐行刷新。逐行刷新后屏障等待降至 0.06 ms，但 WindowServer 的内核 CPU 时间从每轮 6.2 s 增至 11.7 s，轻量动画的长帧比例由 16% 升至 34%。写合并映射出现 GPU 页保护错误。Linux amdgpu 在 Renoir 上同样用 SDMA 更新图形任务的页表，因此该方案已移除，相关数据位于 `build/uma-cpupt3-20261122` 和 `build/uma-wcpt-20261122`。

映射复用见下文。仍需新建的散页映射，后续可在 `mapVMPT` 的叶循环内合并为 `WRITE_LINEAR` 命令。

### 原生缓冲池

`IOAcceleratorFamily2` 为以系统内存为主存储的 GPU 提供 `IOAccelBufferMemoryDescriptorPool2`。`IOGraphicsAccelerator2::start` 只在驱动配置的 `0xc90` 第 21 位置位时建池，AMD 驱动不置该位。统一内存在同一检查点为 Renoir 加速器建池，不改动该配置位，因此其他依赖该位的原生行为保持不变。

池按 1 MiB、256 KiB、64 KiB、16 KiB 和 4 KiB 五档缓存已锁页的可分页、可丢弃描述符，单次请求上限 24 MiB。统一内存的主分配和数据缓冲区经 `IOAccelSysMemory::withOptions` 的原生池路径取得预先锁页的描述符组合。资源释放时，`orphanIt` 把描述符连同所属任务和最后的 GPU 事件放回池中，同一任务复用时无需重新清零。超出上限或池中无可用描述符时走原有可分页路径。非统一内存的系统内存分配不经过池。池的补充由原生定时器在加速器锁内完成。

改动前，WindowServer 每次创建或销毁窗口缓存都要新建可分页内存、逐页缺页清零并锁页，释放时再解锁。2026 年 11 月 22 日的内核栈采样中，锁页和解锁分别占 WindowServer 内核 CPU 时间的 15.5% 和 13.5%。`pool_allocations` 记录由池直接提供的 backing 数。

### 映射复用

`IOAccelMemoryMap` 的页表项提交后会给所属 backing 增加一次锁页引用，`release_pte` 时减去。WindowServer 删除资源时，该任务对同一 backing 的最后一个映射随之释放，页表项被清除，锁页引用归零后 backing 立即解锁。几十毫秒后新资源引用同一 IOSurface 缓存，又要新建映射、分配 GPU VA、重新锁页并重写全部页表项。2026 年 11 月 22 日的跟踪中，25 秒内 3228 次解锁有 3043 次来自这种映射释放。

`IOAcceleratorFamily2` 已有对应机制。`IOAccelMemory::setFlags(2)` 置位后，`IOAccelMemoryMap::release` 把任务的最后一个映射连同 GPU VA 和已提交的页表项移到任务的孤儿列表。`createMappingInTaskAtAddressLength` 为同一任务重新映射同一 backing 时直接取回该映射，`prepare` 见页表项仍有效便不再提交。AMD 驱动不开启这一机制，统一内存为纹理、表面缓存和数据缓冲区的主 backing 开启。

不置该标志时，原生代码也会把 GPU 工作尚未完成的映射移入孤儿列表，置位后空闲映射同样保留。

孤儿映射保留锁页引用，回收沿用原生路径。GART 回收器每 250 ms 检查一次，按 20 s、10 s、5 s 三档空闲时间解锁不再使用的 backing，三档分别在锁页总量超过 32 MiB、内存的 1/16 和 1/8 时生效。`unwire` 清除该 backing 全部映射的页表项，包括孤儿映射，映射对象和 GPU VA 保留到复活或释放。统一内存对非 IOSurface 纹理和数据缓冲区的 `Empty` 请求，在 prepare 计数为零且 GPU 事件完成时立即经 `unwire` 解锁，其余情况由原生代码记录状态。任务 GPU VA 不足时，`freeWaitToAllocGPUAddress` 先回收孤儿映射。长期存活的 backing 的孤儿映射没有单独的数量或时间上限，GPU VA 保留到 backing 释放、VA 不足或任务退出。

backing 只剩孤儿映射引用时，已无对象能复活这些映射，原生代码调用 `IOAccelSysMemory::orphanIt`。池化 backing 在这里把描述符退回池中，而孤儿映射的页表项可能仍然有效。统一内存在调用 `orphanIt` 前，按 `IOAccelMemory::release` 的做法先释放 GPU 事件已完成的孤儿映射。若全部释放，backing 随之销毁，不再进入孤儿池。仍有 GPU 工作的映射按原生规则保留。同一任务可以先取回这些描述符，并继承合并后的 GPU 事件，该事件只覆盖旧的 GPU 工作，旧页表项的清除和 TLB 失效顺序尚未实测。原生驱动对忙碌映射本来就走这条路径。2026 年 11 月 22 日在未置位的构建上跟踪 12 秒，WindowServer 取回孤儿映射 1560 次，其中 1527 次页表项仍然有效。

映射查找只按任务匹配，不比较请求的 GPU 地址。原生代码为固定地址建立映射后会清除该标志，但已有的孤儿映射仍可能被取回。统一内存在固定地址请求前先释放该 backing 中 GPU 事件已完成的孤儿映射。统一内存的主分配本身不接受指定地址的资源。

2026 年 11 月 22 日 kext `8C62ED5A` 单次开机验收（`build/uma-mapreuse-20261122`），与当天开启统一内存的旧构建相比：每轮动画测试的页表提交由 4741 次降至 1686 次，WindowServer 的页表命令缓冲区由 33758 个降至 4519 个，中位长度由 4080 降至 602 个双字。轻量动画中图形提交的调度停留时间中位数由 6.47 ms 降至 0.01 ms，p90 由 18.5 ms 降至 0.50 ms。超过 25 ms 的帧间隔比例：轻量动画 2.0%（旧构建约 15%，关闭统一内存 2.5%），调度中心和全屏 16.9%（旧构建约 26%，关闭统一内存 54%），全部系统场景 7.3%（旧构建约 20%）。锁页总量测试期间最高约 490 MB，23 GiB 压力探针后回落到 2 MB。全部正确性探针通过，未出现 GPU 重置。

池中的 backing 收到 `Empty` 请求时同样立即解锁。描述符退回池时，原生代码丢弃已清空的描述符。

`pageon` 需要临时复制时沿用驱动原生的显存 staging。2026 年 11 月 22 日在动画测试和全部探针期间跟踪 `pageon`，其 staging 分支一次也未进入，此前 138 份计数器记录中可分页 staging 同样从未触发，因此该路由已移除。

## 启动配置

该版本的路由限于 Renoir 和 macOS 构建 `25G332` 对应的两个 kext UUID。本机从系统 EFI 的 `EFI/OC/Kexts/NootedRed-MappingReuse.kext` 启动，`Kernel → Add` 中将 `Lilu.kext` 排在它前面。

日用配置使用 `-NRedUnifiedMemory`。`debug.nred_uma.active` 为 `1` 时驱动路由已安装，系统日志出现 `Creating the native buffer pool` 表示池已建立。固件回滚入口为 `F9 → Boot from EFI File → \EFI\NRedBackup\OpenCore.efi`。

本机按默认配置使用原生驻留预算。预算回归测试可给测试 EFI 加 `nred-uma-wired-mib=768`，用较小的工作集触发原生回收。

启用 `-NRedUnifiedMemory` 时，特殊分配沿用原生显存分配器，不安装扩展的 `allocateLargeBlocks`。OpenGL 的上游低显存预算修正提供兼容回退。旧版 RAM 分类放置、Metal 显存预算覆盖和 4K scanout 布局实验见分支 `codex/gpu-page-lifecycle-20261115` 的提交 `075a5d1`。

## 实机验收

每轮记录 macOS 构建、源码提交及工作区改动、`kmutil showloaded` 中的 kext UUID、启动参数和测试前后计数器。下文引用的 `build/` 日志保存在本机，由 Git 忽略。

在仓库根目录构建探针并运行小规模读回。

```sh
xcrun clang -fobjc-arc -O2 -Wall -Wextra -Werror \
  -framework Foundation -framework CoreGraphics -framework IOSurface -framework Metal \
  tools/x5000_uma_probe.m -o /tmp/x5000_uma_probe
codesign -s - /tmp/x5000_uma_probe
sysctl debug.nred_uma
/tmp/x5000_uma_probe --groups 1 --require-budget --require-surfaces --surface-ipc --layouts
```

保持 4K 桌面活动，运行 `/tmp/x5000_uma_probe --groups 36 --require-budget` 验证容量。用 768 MiB 驻留预算再运行 `/tmp/x5000_uma_probe --groups 36 --idle 10 --require-budget --require-reclaim --require-surfaces --surface-ipc` 验证回收。探针保留私有缓冲区和渲染目标，检查 `bytes_live` 增长超过 512 MiB 并逐字节校验 GPU 读回。回收测试要求 `same_va_recommits` 增长，并核对缓冲区 GPU VA。本机对象级核验脚本位于 `build/x5000-unified-memory-boot/reclaim-lifecycle.d`，追踪相同 backing 的 PTE 移除、解除锁页、重新锁页和原 VA 提交。`--require-surfaces` 检查可分页缓存计数。`--surface-ipc` 在施加内存压力前将同一 IOSurface 映射到两个进程，两边保留原纹理直到压力解除，并校验 GPU 写入和 CPU 重写后的可见性。`--layouts` 验证私有 heap 的缓冲区、颜色纹理和深度纹理，并验证带深度和模板附件的 4x MSAA resolve。驱动计数器按设备汇总。

`--managed` 使用 `MTLStorageModeManaged` 缓冲区和纹理，校验 CPU 写入、GPU 读回、GPU 修改后的 CPU 同步，以及对原资源的再次上传。`--seed` 设置分配后的首个校验字节，便于并行进程用不同的数据验证相同 GPU VA 的任务隔离。

`--purgeable` 校验 Metal 的可丢弃状态及其恢复，检查 `Empty` 后能在同一缓冲区 GPU VA 重写并读回，并要求 `purge_unwires` 增长。配合 `--managed` 验证 CPU 与 GPU 两份 backing。AMD Metal 在跟踪中把私有渲染目标的 `Volatile` 请求转换为 `NonVolatile`，探针对这一情况校验内容保留。

`--hotplug 300` 保留原资源，等待外接主显示器离线再恢复到原分辨率，然后进行 GPU 读回。配合 `--surface-ipc` 同时验证重连前已在两个进程建立的 IOSurface 映射。外屏验收还需现场确认拔线前持续有画面、重连后桌面恢复，并且能看到画面变化。

OpenGL 探针用单次绘制同时采样五张纹理，验证超过旧列表预算的命令和多组资源读回。正式用例在活动桌面下保留 816 MiB 的纹理，每次绘制引用 272 MiB。探针要求两个预算接入点生效、设备收到新预算、GPU 完成读回，并观察到超过 512 MiB 的 pageable 主 backing 增量。`--smoke` 用较小的纹理检查接入。

```sh
xcrun clang -std=c17 -DGL_SILENCE_DEPRECATION -O2 -Wall -Wextra -Werror \
  -framework OpenGL tools/x5000_uma_gl_probe.c -o /tmp/x5000_uma_gl_probe
codesign -s - /tmp/x5000_uma_gl_probe
/tmp/x5000_uma_gl_probe --smoke
/tmp/x5000_uma_gl_probe --layouts
/tmp/x5000_uma_gl_probe --buffers
/tmp/x5000_uma_gl_probe
```

`--buffers` 为九个 64 MiB VBO 分别写入并读回首尾标记。对纹理模式或 `--buffers` 加 `--wait-for-pressure` 时，探针保留原资源，收到标准输入的一行空行后重新读取。VBO 用例随后从 CPU 更新原缓冲区并校验 GPU 可见性。驻留验证需要配合映射跟踪核对每个 backing，统计接口的计数包含同一设备上的其他进程。

### 合并后的验收状态

新构建需重跑本节全部用例，并观察到 `pool_allocations` 增长。以下实机记录对应各自标注的旧构建。

新构建的实机验收需重跑本节读回和回收用例。整机睡眠恢复、实际 GPU 重置和从可见桌面开始的外屏热插拔，都应保留原资源并在恢复后校验读回。

`python3 tools/test_x5000_memory_policy.py` 检查放置策略、驻留状态转换和预算计算。物理热插拔和实际 GPU 重置需要另外实测。

### 早期资源 backing 验收

2026 年 11 月 21 日 00:01 CST 从系统 EFI 启动，kext UUID 为 `F7C4DF32-3BEC-396C-873E-0278F5CAA553`，macOS 构建为 `25G332`。`debug.nred_uma.active` 为 `1`，原生驻留上限为 22 GiB，Metal 建议工作集为 16.5 GiB。

本次验收日志位于 `build/x5000-unified-memory-boot/qa-F7C4DF32`。

- 在原生驻留上限下运行 23 GiB Metal 工作集，同时保留 816 MiB OpenGL 纹理、576 MiB VBO 和跨进程 IOSurface。736 个私有缓冲区、736 个渲染目标、15 张 GL 纹理、9 个 VBO 及同一 IOSurface 的 3 个映射均观察到 PTE 移除、解锁、重锁及在原任务和 GPU VA 提交。GPU 读回和跨进程读写校验均通过，分配失败计数为零。`object-validation.log`、`metal-pressure-workload.log`、`gl-vbo-workload.log` 和 `gl-texture-workload.log`
- 九个 VBO 的原生客户端导入描述符均完成锁页、解锁及后续上传所需的重锁。内存压力后读取九个原缓冲区，再从 CPU 更新首尾标记，GPU 读回全部通过。`client-buffer-validation.log` 和 `client-buffer-workload.log`
- 同时保留八个 Metal `Managed` 缓冲区和八张 `Managed` 纹理。施加驻留压力后，16 个主映射均恢复到原任务和 GPU VA，GPU 读回、同步到 CPU 和重新上传均通过。`managed-validation.log` 和 `managed-pressure-workload.log`
- 20 个 Metal 进程在各自的任务页表中映射相同的 GPU VA，并分别读回不同内容。GFX 和 MM VM hub 都观察到 VMID 被不同任务复用，原生驱动在新分配完成前生成失效请求。`vmid-validation.log` 和 `vmid-workload.log`
- 4K 桌面完成分辨率切换，提交了 96 帧全屏 Metal drawable。WindowServer 的离屏主映射使用系统内存，采集到的 775 次扫描输出面绑定均使用显存。4K HEVC 播放通过。显示电源状态从 `4` 降到 `1` 后恢复至 `4`，WindowServer 进程持续运行，熄屏前创建的 GPU 资源和跨进程 IOSurface 均通过读回。`display-and-layout-workload.log`、`special-placement.log`、`video-workload.log` 和 `display-power-workload.log`
- 4K 可显示 IOSurface 使用原生显存，使用相同尺寸的离屏 IOSurface 作为对照时走可分页系统内存。两组 GPU 读回均通过。`scanout-surface-trace.log` 和 `scanout-surface-workload.log`
- OpenGL 4x MSAA 和深度模板用例中，带独立辅助描述符的主分配使用原生显存。Metal 私有 heap 的缓冲区、颜色和深度纹理、4x MSAA resolve 和跨进程 IOSurface 双向读写均通过。`special-placement.log` 和 `postboot-regression.log`
- Metal 的 2048² 颜色纹理附带 64 KiB 内联 DCC 元数据。跟踪确认整个主分配使用可分页系统内存，GPU 读回通过。`metal-dcc.log` 和 `metal-dcc-workload.log`
- OpenGL 的 `GL_STORAGE_CACHED_APPLE` 和 `GL_STORAGE_SHARED_APPLE` 用例均经过原生客户端描述符和可分页主 backing，首尾像素读回通过。`gl-client-storage-trace.log`、`gl-client-storage-cache-workload.log` 和 `gl-client-storage-shared-workload.log`

2026 年 11 月 21 日约 06:58 CST 的热插拔试验中，用户在拔线前已看到黑屏，外屏画面验收未通过。防止空闲熄屏的断言在等待拔线期间有效。系统记录同一 framebuffer pipe 离线及重新上线，重新识别到同一台 4K 显示器。重连后四组 Metal `Managed` 缓冲区和纹理及跨进程 IOSurface 均通过读回，WindowServer 和系统继续运行。黑屏起点及拔线前的 DP 链路状态尚未采集。`hotplug-u843RQ/trace.log`、`hotplug-u843RQ/workload.log` 和 `hotplug-u843RQ/display-events.log`

重连后的变色窗口测试中，驱动提交并完成了 711 次显示事务，记录的 scanout 绑定全部使用显存。随后进行一次短暂熄屏和唤醒，显示电源状态恢复到 `4`，DP 接收端恢复四条 HBR3 通道锁定，显存翻帧继续。两轮变色测试各有 31 帧收到呈现时间戳，首帧报告时间为 `0`。面板上的实际画面仍待现场确认。`hotplug-u843RQ/presentation-trace.log`、`hotplug-u843RQ/display-link.log`、`hotplug-u843RQ/display-wake-trace.log` 和 `hotplug-u843RQ/display-wake-workload.log`

Apple 图形诊断保留的 HDCP 认证启动事件从 06:31:27 CST 起每约 3.2 秒出现一次，重连后仍在出现。07:14 CST 的接收端 HDCP `RxStatus` 值为 `0x10`，对应加密同步失效。07:19 CST 起的约一分钟观察中，30 次接收端采样均为四条 DP 通道锁定且 HDCP 错误位清零，90 帧变色图案中 89 帧收到呈现时间戳，分配失败与 scanout 拒绝计数均为零。该状态与面板黑屏的时间关系仍待现场复现。`hotplug-u843RQ/graphics-diagnose.log`、`hotplug-u843RQ/physical-observation.log` 和 `hotplug-u843RQ/physical-observation-frames.log`

07:31 CST 用户报告 Dell 仍黑屏，摇杆菜单没有出现。此时系统显示电源状态为 `4`，识别到 4K 外屏，图形诊断报告四条 DP 通道锁定及 HDCP `RxStatus=0x10`。显示器断电再接回后，驱动重新识别到同一显示器，随后接收端 HDCP 状态为 `0x00`。临时切到 1080p 并自动恢复 4K，实际像素时钟从 533.25 MHz 降至 159.84 MHz，再回到原值。Metal、深度纹理和跨进程 IOSurface 的 GPU 读回均通过。仍需现场确认面板能否显示自身菜单及桌面。`hotplug-u843RQ/black-live.KR1unx`

07:46 CST 用户仍报告黑屏。系统截图有正常的非黑像素，四条 DP 通道锁定，HDCP `RxStatus=0x00`。Dell 通过 DDC/CI 报告亮度为 `0/100`。07:48 CST 将亮度调到 `50/100`，即时读回和 07:49 CST 复查均为 `50/100`。用户随后确认能看到桌面。UMA 在同一次启动中保持开启，分配失败和 scanout 拒绝计数均为零。仍需完成一次从可见桌面开始的热插拔验收。`hotplug-u843RQ/black-live.KR1unx/ddc-brightness-restoration.log`

对照配置 `hotplug-u843RQ/uma-off-config.plist` 从本机 EFI 配置生成，仅移除 `-NRedUnifiedMemory`，仍使用同一份 kext。该配置已写入 `EFI/OC/config.uma-off.plist` 并通过 `ocvalidate`。OpenCore 仍从 `EFI/OC/config.plist` 启动。关闭该开关时 X5000 会回到兼容显存分配路径，等待实机对照。

本轮 IOSurface 经过 `pageoff` 和 `pageon`，copy kind `8` 的 staging 标志均为 `0`。分辨率切换和全屏 4K drawable 触发了 scanout DCC 资格检查。实际启用 DCC 的扫描输出、整机睡眠恢复和 GPU reset 尚待实机覆盖。上一版整机睡眠试验记录了 `kPMPCIUnsupported`，试验后恢复原电源设置。`qa-F7C4DF32/residency-edges.log`、`system-sleep-kernel-7E844CDE.log` 和 `system-sleep-isolated-7E844CDE.log`
