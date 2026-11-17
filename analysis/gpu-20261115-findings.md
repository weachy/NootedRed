# 2026-11-15：本机 GPU 故障的源码定位与修复

**2026-11-16 11:13 更新：生产 kext 重启后的 GL 和 Bilibili 播放实测通过，详见第 8 节。
最小 GL 提交的因果定位与修复见第 7 节。
本地已同步 weachy `548631e18146a0ff6ad1760bb19e2f245a57608f`（1.2.6 元数据）。
以下早期分析保留其原来的验证范围。本次已验证普通进程 GL、Bilibili AV1、
H.264 和 HEVC Main 8-bit 播放；Main10、4K/60fps 及数小时稳定性尚未验证。**

本机是 Ryzen 7 PRO 4750U / Renoir 1002:1636，512 MiB UMA，macOS 26.7.2 (25G332)。
本次已将本地 weachy master 拉到 `3c5ae6caca1851018eaa9af87a9e89b877f2b5bb`
（v1.2.5），工作分支为 `codex/gpu-page-lifecycle-20261115`。
依赖通过 Surge `127.0.0.1:6152` 拉取，没有修改全局 Git 代理配置。

**下面第 1–5 节记录首版两项修复的分析。首版已安装并重启验证加载，但 20:18 的 B 站
复测出现 VCN0Dec 故障，最终 WindowServer watchdog 重启，因此不能把首版称为修复成功。
续查确认实际使用的 X6000 视频上下文遗漏资源失败处理修补；修复和 CPU 回放见第 6 节。
该缺陷已证实，是否触发本次实机 VM fault 仍需下一次启动验证。**

## 1. 显存回退分配解析了错误的 C++ 重载

`X5000::processKext` 中 `orgAllocateNonContiguous` 声明需要的是：

```text
allocateNonContiguous(element, size, alignment, allocationType,
                      bool, bool, chunkSize, origin, operation)
```

v1.2.5 实际解析的 `...Elementyyyybby...` 则是：

```text
allocateNonContiguous(element, size, alignment, rangeStart, rangeLength,
                      bool, bool, chunkSize, origin, operation)
```

第二种比函数指针声明多一个参数。分配类型被当成范围起点，随后的 `false` 被当成
长度 0，栈参数也随之错位。这条路径在连续分配失败后使用。

本机 `SystemKernelExtensions.kc` 同时存在两个重载：

| 重载 | 本机静态地址 / 文件偏移 |
| --- | --- |
| allocation type，`yy28_eAMD_MEMORY_ALLOCATION_TYPEbby` | `0x4371c26` |
| 显式范围，`yyyybby` | `0x4371a28` |

已核对这两个函数的反汇编。allocation-type 版本先根据类型和对象字段计算真实
VRAM 范围，再调用 range 版本。

更强的 CPU 回放：取本机真实的 174 字节 allocation-type 适配函数，只把它最终调用的
range 分配器换成 CPU 记录器，虚表查询也只返回一个受控布尔值。
伪对象配置为总 UMA 512 MiB、可见范围 256 MiB；输入 origin=7、chunk=128 KiB。
没有调用真实分配器、IOKit 或提交 GPU 命令。

| 受控输入下的行为 | 错误重载 | 正确适配函数 |
| --- | --- | --- |
| range start | `1` | `0x10000000` |
| range length | `0` | `0x10000000` |
| chunk size | `7` | `0x20000` |

这里的伪对象字段和 origin=7 是测试输入，不是对故障时内核对象的运行时抓取。
已加载 release 对应二进制 SHA-256 为
`e0cd7346bf4ca870dc47be37060239d47c9a577365333440c214a7a7cf1e8194`；
与保存的公开 v1.2.5 release 相同，其字符串中没有正确的 allocation-type 重载。

修复包括：

- 改为解析与函数指针声明匹配的 allocation-type 重载。
- 所有回退保留调用方要求的起始对齐，不再降为 4 KiB 对齐。
- `onlyNonVisible` 为真时，回退也限制在 non-visible 范围。
- 细粒度回退保留调用方指定的最小 block size。
- 可用 `-NRedAllocTrace` 记录前 64 次非连续分配尝试，包含结果、size、alignment、
  allocation type、chunk size 和 minimum block size。
- hook 安装成功时公布 `NRedVRAMAllocatorRevision=2`。该标记只证明 hook 版本，
  不证明某次分配已经走过回退。

注意：源文件另一个 `patchIOAccelAllocPages` 仍需解析 range 重载，以定位其内部
`allocPages` 调用。因此修复后的二进制同时包含两个重载名称是正常的。

## 2. 页面补丁错误地使用开机全局计数跳过后续页面

原版 `UserPatches::patchSignaturePage` 用 `signatureApplied`、
`vdaSignatureApplied`、`gvaSignatureApplied` 和 `mtlHwInfoApplied`
等全局状态抑制后续补丁。这些状态活到重启，但签名验证状态属于当前驻留页：
文件页可以被回收并重新载入。

原版对第一个页面打补丁后，会漏掉以后带原始字节的新页面。已使用生产 matcher、
生产 patch method，以及本机真实 GL 页面恢复已知原始补丁位点的方式复现。

修复删除生命周期跳过条件，让每个新验证页按原始字节和上下文重新匹配。
VDA 的匹配数量限制现在作用于当前页；统计不再控制是否打补丁。
保留 vnode 筛选、匹配上下文、页边界、原始 find 字节以及 Metal Gate/Allow 同页配对检查。
带有固定 `pageOffset` 的签名也检查其文件偏移。

可用 `-NRedUserPatchTrace` 记录前 128 次实际写入，含签名类别和 file/cache page offset。
安装成功时公布 `NRedUserPatchRevision=2`。

## 3. 实机测试确定了什么

- 原始 14:36–14:37 故障位于 GFX 通道；首个 pending command 对应哔哩哔哩 GL 上下文。
  没有证据把它归为 VCN 解码器或 Metal compute 故障。
- GL 上下文查询和单源、双源 shader 编译链接通过。
- unordered AppKit drawable 的单色测试在提交路径终止。
  17:51:30、17:54:04 有内核 internal error `0x0, 0xbfe88fc`，
  随后分别在 17:51:37、17:54:11 记录 GFX hang / restart。
- 使用双缓冲 NSOpenGLView 并确认窗口 visible 后，在 AppKit drawable 初始化的提交
  路径再次终止，尚未执行所选的 solid draw。18:21:49 抓到相同 internal error。
  18:21:40–18:23:00 的针对性内核日志中没有 GPU hang / restart 记录。
  因此不能把每一次测试进程退出都等同于 GPU reset。
- 低层 `IOAccelGLContextSubmitDataBuffersExt2` 的调用返回值为 0，但 error 输出字段非零；
  GPUSupport 随后调用 kill-client 回调。该内部错误码的具体来源尚未定位。
- 已在测试进程内部确认补丁字节 `c7 47 78 01 00 00 00`、`83 c8 01` 都存在。
  实际 HwInfo 为 7 CU、1 SE、1 SH，feature flags `0x4d`，
  `GB_ADDR_CONFIG=0x24000042`。因此这些测试不能直接归因于“两处 GL 补丁没打上”。
- 已截获 CPU 生成的 PM4 缓冲，阻断模式在绘制提交入口保存后退出。
  创建 GL 上下文本身仍有初始化提交，不能把 inspect/link 称为零 GPU 提交。
  抓取日志早期标作 `gpu=` 的 context `+0xd70` 字段，后来通过导入桩确认是
  `IOAccelResourceGetClientShared` 的返回指针，并非 GPU 虚拟地址。

## 4. 验证与构建

源码回归直接抽取当前 checkout 的相关函数与声明，内核服务使用 CPU stub：

```sh
python3 tools/test_userpatch_lifecycle.py
python3 tools/test_vram_allocator.py
```

本机真实 GL 快照也参与了页面回归。结果：

| 检查 | 修复后 | 原版 v1.2.5 |
| --- | --- | --- |
| 页面生命周期，AddressSanitizer / UBSan | 757 项断言，0 失败 | 338 失败 |
| 分配器回退与 ABI | 180 项断言，0 失败 | 15 失败 |
| 本机真实内核适配函数 CPU 回放 | 通过 | 记录到错位参数 |

分配器测试的断言总数取决于实际发生的回退次数；原版断言总数是 84。
基线测试刻意复现错误 ABI，但记录器只消费已提供的寄存器参数，不访问缺失栈参数。
修复后测试不走该错误调用。

RELEASE、RELEASEDBG 均已完成 x86_64 kext 编译和链接。
本地 xmake bundle 在 AppleForge 的 ZIP 启动步骤失败；已用 Python zipfile
打包最终 bundle，并检查压缩包 CRC、包内可执行文件与构建产物一致、
Info.plist，以及可执行文件和 dSYM UUID 配对。

最终产物、manifest、源码 patch 和原始记录位于：

```text
installer-staging/verification/gpu-status-2026-11-15/live-tests/
```

其中：

- `build-artifacts/NootedRed-1.2.5-allocator-pagefix-RELEASE.zip`
- `build-artifacts/NootedRed-1.2.5-allocator-pagefix-RELEASEDBG.zip`
- `build-artifacts/build-manifest.json`
- `allocator-abi-replay.log`
- `allocator-enum-adapter.json` / `kernel-allocator-4371c26.asm`
- `allocator-tests-current.log` / `allocator-tests-release.log`
- `page-tests-current.log` / `page-tests-release.log`
- `submit-capture-solid-detailed/` / `submit-observe-ordered-solid/`
- `gpu-hidden-focused.log` / `gpu-ordered-focused.log`

## 5. 尚缺的因果验证

需要启动修复后的 kext，再用上述两个诊断参数记录回退分配与页面写入。
首先确认两个 revision 属性为 2，随后从最小 GL 基线到原始应用场景逐级测试，
对照 allocation trace、GPU hang、pending context 和实际输出。

现有数据没有捕获到 14:36 原始故障时是否执行过错误分配重载或页面重新载入。
不能把 CPU 回归通过称为实机故障已经修复，也不能从能力枚举宣称 H.264 / HEVC /
Main10 已完成逐帧硬解验收。当前不应再用未经核对的寄存器常量或 vtable 改动掩盖故障。

## 6. 首版复测后的续修：VCN 实际使用 X6000，错误分支却只修了 X5000

20:18:36 的首个完整 gpuRestart 报告是 VCN0Dec，首个待处理命令属于 VTDecoderXPCService，
VMID 7，TS 8/9 未完成。MM fault 地址为 `0x400c04000`，写保护/有效性/NACK 故障。
同一时刻 GFX/SDMA 已完成。因此这轮不能再称作原来的 GL/GFX 故障。
随后 WindowServer 等待 VTDecoderXPCService 的图形锁；该线程在
`AMDNavi10VideoContext::removeContext+0x171` 等待解码队列。watchdog 最终触发重启。

当前内核集合中两套 `AMDAccelVideoContext::processCommand` 的关键控制流一致：

- X5000：`0x43451bc`；X6000：`0xbde82f4`。
- 入口 `+0x3e` 将 `this+0x644` 设为 `-6`，`this+0x11d0` 设为零。
- `+0x172` 调用 fetchResource；`+0x17c` 在返回 false 时跳到 `+0x4ff`。
- `+0x4ff` 是成功完成块：写入命令长度、写输出参数，再把错误状态清零。
- `+0x51d` 才是不覆盖错误/长度的退出路径。
- `endCommandStream+0x35` 检查 `this+0x644`，非零时拒绝视频提交。

fork 的旧 `patchVideoProcessCommand` 只修补 X5000；`VCNBridge::newVideo` 则创建 X6000
上下文，实际 VCN 仍保留上述错误分支。新 `VideoCommandPatch.hpp` 统一两侧修补：
在校验入口错误赋值、fetchResource 调用目标、返回值检查、完整成功块和 epilogue 后，
将失败分支直接指向错误退出。X6000 无法匹配时，桥接不进入 ready 状态。
加入 `NRedVideoCommandGuard5000/6000` 标记，以及仅失败时更新、限 8 次的
`NRedVCNResourceFailures`，用于下一轮确认是否实际触发该条件。

`tools/test_video_command_guard.py` 从本机 KC 抽取两套真实函数并在 CPU 上执行；
仅将 memcpy 和 fetchResource 替换为本地 CPU stub，不连接 IOKit、不调用内核分配器、
不向 GPU 提交命令。另执行本机真实 endCommandStream 的错误检查指令：

| CPU 输入 | 原始机器码 | 修复后的机器码 |
| --- | --- | --- |
| 首个资源准备失败 | error=0、length=128，提交检查接受 | error=-6、length=0，提交检查拒绝 |
| 有效的零资源命令 | error=0、length=128，接受 | 同原版 |

两套驱动共 2678 项检查通过（包括控制流损坏、错误目标、截断输入和重复修补识别）。
分配器 180 项、页面补丁 734 项（未传可选 GL 快照）也通过。
RELEASE UUID 为 `C9212E00-CE2F-3CC3-9471-70A03E5A7FB0`；
SHA256 为 `2f3b1a7560100adcafc13556fad8689dc5ed91b377cbc1ba56df4c22e2d8f0ad`。

限制：CPU 回放证明资源准备失败会被原版错误放行，并证明此修复阻止该错误放行。
本次实机没有捕获到首次失败的 fetchResource 返回值，也没有将 fault 地址反查到具体资源，
因此这仍不是“20:18 VCN 故障唯一根因已被证明”的结论。需重启后检查新 UUID/guard 属性、
实际资源失败计数、解码结果和有无 VCN/GFX reset，不能仅凭测试断言数宣布实机恢复。

## 7. 2026-11-16：512 MiB UMA 使 GL 资源列表预算为零

本机 X5000 用户态 GL 驱动的 `_AMDRsrcList_Init`（文件偏移 `0x572a`，50 字节）
从真实 HwInfo 的 `+0x14` 读取总显存 MiB，再计算：

```text
budgetKiB = (totalVRAMMiB << 10) - 0x80000
```

固定扣掉 512 MiB 后，本机 512 MiB UMA 的预算恰为零。
`AMDRsrcList_AddRsrc` 因此拒绝 resident resource，返回 `0xffff`；提交前真实抓取的
资源列表 count 为零，256 项 ID 全零。GPU 虚拟地址已经分配不代表资源已经驻留；
后续命令仍引用这些未列入 BatchPrepare 列表的 scratch/shader 资源。

09:42–09:43 的实验将三顶点 DRAW_INDEX_AUTO 包换成等长 NOP，仍然挂起。
这次内核报告明确给出 CPG 对 `0x400001000` 的 VALID/WRITE protection fault。
首次 pending GL 命令在 draw 前就向该地址 WRITE_DATA / RELEASE_MEM，
随后 WAIT_REG_MEM。资源对象抓取确认它是尚未释放的同步 scratch 分配。
所以故障发生在 draw 之前，修改混合寄存器不能解释或解决它。

另一个表象 `0xbfe88fc` 也有对应的代码路径：
`AMDGFX9GLContext::process_PatchStreamTexBuf` 发现编码的资源索引不小于资源列表 count
时构造 `0xbfe00fc | (tokenTypeByte << 8)`，与空列表一致。

修正为：

```text
budgetKiB = max(totalKiB - 512 * 1024, totalKiB / 2)
```

512 MiB UMA 得到 256 MiB 资源预算；更小 UMA 不再减法下溢；从 1 GiB 起与原策略相同。
这改变的是命令资源列表的预算，不是向系统报告更大的物理显存。

证据与验证：

- `tools/test_gl_resource_budget.py` 从生产 `GLResourceBudgetPatch.hpp` 提取机器码，
  对已核对的宿主驱动 CPU 代码页做 SHA-256 校验后，执行真实 Init 和 AddRsrc，
  外部调用替换为 CPU stub。35 项检查通过。原始 512 MiB 返回 index=65535、
  count=0；修正后返回 index=0、count=1。包含 128–16384 MiB 的结构及预算检查，
  以及 scratch 接受、引用计数、去重和读写标记验证。没有 GPU 提交。
- 进程私有代码替换后、绘制提交前阻断的抓取，预算为 262144 KiB，资源 count=4，
  ID 为 `0x80000004, 0x80000006, 0x80000005, 0x7`。
- 09:55 的实机 FBO 单色用例返回成功；09:57:25–09:57:48 的实机用例包括原先失败的
  初始 drawable swap，以及单色、普通 alpha、SRC1_COLOR、SRC1_ALPHA 四种绘制。
  回读分别为 `(255,0,0,255)`、`(128,0,128,128)`、`(64,0,64,255)`、
  `(64,32,223,255)`，全部与 CPU 参考一致，最大差值为零。进程正常返回 0，
  监控期间没有新增 GPU 报告，内核监控日志没有匹配到故障事件。
- 通过的实机实验只启用资源预算修正；没有强制 CB 寄存器、没有抑制 draw。
  实验补丁只写入测试进程的私有页，没有修改系统 GL 驱动文件。
- 生产代码在已有原生页面验证后端中精确匹配完整 50 字节函数，拒绝多重匹配与跨页匹配，
  仅用于 GL vnode，保留 resolver 关闭开关，按每个新验证页重试。
  `NRedGLResourceBudgetApplied` 只统计成功写入；启动后还需核对该属性和实际进程中的字节。
  旧系统的 Lilu 用户态补丁后端未扩大本次修改范围。
- 加入预算修复后，页面生命周期测试（含宿主 GL 快照）1001 项、分配器 180 项、
  X5000/X6000 视频资源检查 2678 项全部通过。

本次记录位于
`installer-staging/verification/gpu-status-2026-11-16/gl-initial-cb/`，
包括 `live-fbo-nop-draw`、`resource-before-hang`、`budget-fixed-blocked`、
`budget-fixed-live-solid` 和 `budget-fixed-live-swap-all`。
生产回归、构建和安装记录位于同级 `gl-resource-budget/`。

上述对照证明了本机最小 GL 失败路径的因果关系。此阶段安装前尚未验证完整 B 站播放、
长时间负载及独立 VCN 解码故障；后续重启实测见第 8 节。不要自动重启。

## 8. 2026-11-16 重启后的生产 kext 实机验收

本次启动时间为 10:50:37，运行中的 NootedRed 版本为 1.2.6，
UUID 为 `73317750-B4F3-3F08-BE81-B801B462E455`，与安装包一致。
`NRedGLResourceBudgetApplied=1`，分配器和页面补丁 revision 均为 2，
X5000/X6000 视频资源 guard 均为 1，`NRedVCNResourceFailures=0`。

10:57:38 的普通 GL 测试进程未使用 DYLD 注入、进程私有预算替换、CB forcing 或
draw suppression。原先失败的初始 drawable swap 及单色、普通 alpha、SRC1_COLOR、
SRC1_ALPHA 四项真实绘制全部通过，像素最大差值为零，返回码为 0。

Bilibili 原生客户端的实际播放统计：

| 码流 | 分辨率 / 帧率 | 观测掉帧 | 验证范围 |
| --- | --- | --- | --- |
| AV1 | 1920×960 / 29.966 fps | 1 / 10369 | 正常出画和持续播放；不作为 VCN 硬解通过依据 |
| H.264 | 1080×1920 / 30 fps | 0 / 638 | 短片播放完成并自动切集，VCN 有提交和完成 |
| HEVC Main 8-bit | 1920×1080 / 29.97 fps | 5 / 8441 | VCN 有提交和完成；暂停恢复、全屏进出、快进后继续播放 |

HEVC 在上述交互测试前的观测为 0 / 1821 掉帧，最终掉帧比例约 0.059%。
播放策略选项需要重新打开视频才生效，表中编码均由实际播放器统计确认。
测试结束已恢复原来的默认编码偏好，并将视频暂停。

10:57:38 至 11:13:46 两段采样共 438 条，所有样本 `recoveryCount=0`；
没有新增 gpuRestart 或 WindowServer 故障报告，10:56 起的内核日志查询没有匹配到
GPU fault/reset/hang 事件。此次验证使用已安装的生产 kext，没有再次改动 EFI。

本轮通过的是上述实际播放场景，未验证 HEVC Main10、4K/60fps、数小时稳定性，
也没有做解码像素与软件对照。未采集 GL 资源列表预算峰值和拒绝计数，因此不能从
本轮低掉帧推断 256 MiB 对任何工作负载都足够，或断言此前独立 VCN 故障已全部根治。

验收明细、原始日志和采样位于：
`installer-staging/verification/gpu-status-2026-11-16/gl-resource-budget/post-reboot-105738/`。
`runtime-verification.json` 与 `summary.md` 记录本轮通过范围及未验证项目；
安装清单已关联本次实机验收。
