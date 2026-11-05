# 模拟执行环境扩展 1.1.0

本扩展将 enkernel 自身的环境获取方法接入现有 lsdriver，共用原来的共享内存请求、握手、连接和生命周期。接口不要求在内核复刻应用现在的 `pidfd_getfd → fstat/pread/ioctl` 流程；KGSL 直接从目标 task 的 fdtable 取得文件引用，线程、页表和信号直接读取内核状态。

来源固定为 `enenH/enkernel` 的 `1c3eb28b4cae3302714d96716aab55bcaf331fc6`。`source.json` 记录所用上游文件、固定 Google 内核参考文件和当前基线模块的 SHA-256。保留原始请求结构与命令数值的选定子集在 `upstream_abi.h`；所有适配都在本目录，不修改作者的 `driver.h`、`driver_loader.h`、`io_struct.h` 或 `version.h`。

主机测试和 Android ARM64 桥接编译已通过；实际内核编译结果、配置和来源分别记录于模块对应的 `build.json`。交付按 6.1.138、Android16 KMI 5 和 KMI 6 区分。应用 `Runtime/Services` 已按驱动选择环境提供者：作者原版保持 `RemoteServices`，自编译 cached / nc 使用 `KernelServices`，不回退用户态环境系统调用。Unicorn 指令模拟器继续在用户态运行。模块实机查询与真实游戏验收另行记录。

## 已接入接口

| 命令 | 用途与方法 |
|---|---|
| `SGAME_SIM_COMMAND_CAPS` | 扩展版本、实现的命令族、数据包大小、4096 项 ID 上限及 cached / nc 读取模式 |
| `SGAME_SIM_COMMAND_PROCESS` | 读取目标 task 的 `start_boottime`，作为本次模拟会话的进程身份 |
| `SGAME_SIM_COMMAND_FILE_READ8` | 持有目标 memfd 的一份文件引用，从位置 0 单次读取 8 字节，不移动目标 `f_pos` |
| `SGAME_SIM_COMMAND_FILE_STAT` | 持有目标文件引用并返回固定的 AArch64 128 字节 stat ABI |
| `MEM_IOC_GET_PID` / `MEM_IOC_GET_PIDS` | 按 task comm 查进程；多 PID 返回数量与总数，保留截断信息 |
| `MEM_IOC_GET_MODULE_BASE` / `BASE2` / `PATH` | 扫描目标 VMA 的文件名，返回模块地址跨度或路径；BASE2 可排除过大的单个 VMA |
| `MEM_IOC_GET_TLS` | 指定 TID 的保存 TPIDR_EL0 |
| `MEM_IOC_GET_THREAD_TID` / `TIDS` | 按线程名查询；优先当前在 CPU 上的线程，再按累计运行时间选择，保留原作者跳过线程组 leader 的语义 |
| `MEM_IOC_GET_THREAD_STACK` | 查找 `stack_and_tls:<tid>` 的连续匿名可读写 VMA，要求包含该线程 TLS |
| `MEM_IOC_EXECUTE_PACGA` | 安装目标任务 APGA key 执行硬件 PACGA，然后恢复当前任务 key；全过程关闭本地 IRQ |
| `MEM_IOC_QUERY_MAPPING` | 持有 mmap 和页表锁，返回单地址物理位置、叶子粒度与 VMA 权限；不触发目标缺页 |
| `MEM_IOC_QUERY_ACCESS` | 区分未映射、驻留、未驻留及不支持的页；未驻留时仍返回 VMA 权限 |
| `MEM_IOC_QUERY_SIGNAL` | 在 sighand 锁下获取处理函数、flags、restorer、mask、blocked，另采样 altstack 和所属 TGID |
| `DFM_IOCTL_KGSL_QUERY` / `VALIDATE_FD` | 直接持有目标 PID/FD 的文件引用并识别 KGSL |
| `DFM_IOCTL_KGSL_QUERY` / `SYNCSOURCE_PEEK` | 解码厂商 CREATE/DESTROY 的 ARM64 调用点确定布局，读取 IDR 的 `XA_FREE_MARK`，预测最低空闲 ID |
| `DFM_IOCTL_KGSL_QUERY` / `GPUOBJ_INFO` | 通过准确的厂商函数类型调用固定、只读的 native INFO handler，使用内核缓冲区 |

内存读取继续使用已有的 Swap 读取接口，驻留页按构建模式选择作者的线性映射读取（cached）或 `MT_NORMAL_NC` PTE 映射读取（nc）；换入的 Swap folio 仍使用内核一致性映射复制。安装系统调用入口、隐藏、触摸、凭据和其他无关功能没有引入。旧的 caller-fd syncsource 操作由目标 PID/FD 查询覆盖其只读能力，不使用内核分发线程自身的 FD。

## 传输与桥接

外层操作号为 `SGAME_SIM_REQUEST_OP = 0x53494d`。`request_obj` 的布局不变：

- `vmemrw_info.user_buffer`：16 字节 `sgame_sim_header`，紧跟所选请求结构。最大数据包 1312 字节，小于已有的 4096 字节缓冲区。
- `vmem_info` 的前 16384 字节：仅用于 KGSL 排除 ID 列表。原枚举 API 在同一把锁内返回值拷贝，不保留对此临时区的引用。
- `status`：返回 `0` 或负 errno；按原来的 `kernel/user` release/acquire 标志交接。

分发器先复制头、指定长度的 payload 和 ID 列表，再调用处理函数。分发线程没有客户端用户地址空间，因此处理函数接收内核私有参数，不能照搬上游 ioctl 层的 `copy_from_user`。ID 列表支持完整 4096 项，在整个查询中只持有一份目标文件引用。目标 task、mm、file 都按次取得和释放，不增加跨请求目标对象缓存。

`client.hpp` 是独立的 C++17 桥接模板，在作者 `Driver` 正常握手成功后附加；所有调用使用同一个 `Driver::request_lock`。示例（工程需先按现有方式构造 `Driver`）：

```cpp
#include "simulation/client.hpp" // 将 kernel/lsdriver-swap 加入 include 路径

sgame::simulation::Client<Driver> env(driver);
sgame_sim_caps caps{};
int status = env.capabilities(caps);
// status == -ENOTTY 表示当前模块尚无本扩展。

mem_get_tls_req tls{};
tls.pid = target_tid;
status = env.get_tls(tls);

dfm_kgsl_query_req query{};
query.abi_version = DFM_KGSL_QUERY_ABI_VERSION;
query.op = DFM_KGSL_QUERY_SYNCSOURCE_PEEK;
query.pid = target_pid;
query.target_fd = target_kgsl_fd;
query.min_id = 1;
const uint32_t simulated_ids[] = {1, 3, 8}; // 正数、升序、唯一
query.excluded_count = 3;
// excluded_ids 字段保持 0，数组由桥接传入共享临时区。
status = env.kgsl_query(query, simulated_ids);
// status == 0 时，再检查 query.out_errno；成功 ID 是 query.out_id。
```

桥接调用前将 status 设为 `-ENOTTY`，兼容旧模块对未知操作不写 status 的行为；不静默回退到用户态系统调用。生命周期仍由原 Driver 管理。请求沿用原同步等待流程，不另加一套连接、fd 安装或超时取消协议。

## 返回语义与适配边界

KGSL 的负 status 表示 ABI、目标进程、符号或布局等基础错误，`out_fail_stage` 保留具体阶段；status 为 0 时，`out_errno` 才表示模拟调用的目标 errno，例如 `EBADF`、`EINVAL`、`ENOSPC`。一次 GPUOBJ_INFO 不调用通用 file ioctl 分发，也不分配、导入或修改对象。syncsource 结果是采样，不预留 ID；没有调用 CREATE/DESTROY。

`caps.capabilities` 表示已实现的协议命令，不保证所有厂商功能和硬件可用。命名栈依赖 `CONFIG_ANON_VMA_NAME`，PACGA 通过 `out_supported` 报告配置和硬件支持。信号 altstack 或映射并发变化可能返回 `-EAGAIN`。KGSL 保留上游按实际指令识别布局的方式，无法识别时返回不支持和阶段信息；运行时布局缓存沿用 KGSL 在本扩展生命周期内保持驻留的假设。

PACGA 是泛认证指令，不能替代模拟器里语义不同的 PACIA。物理位置只是瞬时采样，不固定页、不改变 COW，也不能据此认定两个私有映射具有共享写语义。应用沿用原来的 PACIA 已知答案和 VM 私有信号处理语义，线程 TLS、memfd 内容、stat、KGSL 对象信息与进程身份由新接口提供。

GKI 兼容适配：`kallsyms_lookup_size_offset` 没有模块导出，改由已导出的 `sprint_symbol` 获取相同的符号大小/偏移；GKI KMI 清单不导出 `xas_find_marked`，本目录保存固定内核源码中相同算法的只读子集，继续读取分配标记。`kernel_read` 与 `vfs_getattr` 在 GKI 中没有模块导出，通过现有符号查询取得准确类型的函数指针；构建会核对它们存在于固定内核 `System.map`。没有为了间接调用 handler 关闭 CFI。

## 重放、构建与验证

```bash
python3 scripts/prepare_swap_driver.py --output build/driver-swap/simulation-prepared
bash scripts/test_driver_simulation.sh
bash kernel/lsdriver-swap/build-6.1.138.sh /path/to/work "$PWD/build/driver-swap/simulation-prepared"
bash kernel/lsdriver-swap/build-6.12.23.sh /path/to/work-a16-5 "$PWD/build/driver-swap/simulation-prepared"
bash kernel/lsdriver-swap/build-6.12.58.sh /path/to/work-a16-6 "$PWD/build/driver-swap/simulation-prepared"
```

准备脚本应用 0001—0005 补丁，复制本目录，记录逐文件 SHA-256，并确认四个原始协议文件未改。构建脚本在开工前核对准备源码哈希，在产出后核对实际导入符号、vermagic、Swap 和 simulation 元数据。`sgame-source.json` 与 `build.json` 分别保存源码来源和实际构建结果。

本地主机测试编译生产 `dispatch.c`、`kgsl_query.h`、`kgsl_idr.h` 、`xarray_walk.h` 与 `environment.c`，内核对象和 API 用替身提供。覆盖请求快照、旧模块探测、原请求与扩展并发、目标文件锁和引用、KGSL 基础错误/目标 errno、4096 项 ID、已占用 NULL ID、树边界、并发重试以及 2048 组占用模型比对；新增进程身份、目标 FD 被替换、一次读取、目标位置不变、stat ABI 与错误/引用释放测试；使用 ASan/UBSan。Android NDK 以未修改的真实 `driver.h` 显式实例化所有桥接方法，核对 ARM64 ABI。

`scripts/test_driver_swap.sh` 覆盖 6.1/4KB、6.12/4KB、6.12/16KB 的读取和连接 API，另测 6.12 页表更换、I/O 期间页表移除及大 folio 尾页。原有 105 项 Swap 读取与 2 项初始化失败、5 组核心初始化/卸载、9 组连接生命周期，以及本地加载器回归保留。两种驻留读取模式都执行读取测试。`test_sg6_automatic.sh` 将同一批捕获输入分别通过原测试服务与生产 `KernelServices`/共享传输执行，覆盖自动发现、历史族、真实采样、编码/解码/诱饵恢复及环境失败恢复。本地主机测试不包含真实 PAC 指令、真实信号/页表锁或厂商 KGSL 指令布局的运行验证。各模块的实际内核编译结果见对应 `build.json`；设备验证单独记录。

6.12 按 Android16 KMI 5 / 6 分别使用 6.12.23 / 6.12.58 的固定源码构建；默认交付均为原生 4KB。KMI 代数和 4KB / 16KB 页配置独立。内核连接层保留完整原生页及协议地址页内偏移的支持，但原作者 `driver.h` 中的固定 `mmap` 地址只有 4KB 对齐，原生 16KB 用户态仍需独立适配；当前不交付 16KB 模块。
