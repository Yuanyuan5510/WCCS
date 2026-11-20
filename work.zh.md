# 《WCCS 多平台库创作手册 v1.0》

**适用语言**：C / C++ / Rust / Python / C# / Node.js  
**核心原则**：**一个 C 核心，六个薄绑定层**。算法逻辑只写一次，其余语言只做 ABI 翻译与包封装。


## 0. 架构决策：为什么必须是 C 核心

多平台库最稳定的契约不是“每个语言各写一遍算法”，而是**一个版本化的 C ABI**。C ABI 是 Go、Java/JNI、C#、Dart、Swift、Kotlin、Zig、R 等语言的通用交汇点，也是 C++/Rust 可以直接消费的稳定层。Radix 的实践验证了这条路径：核心与接口分离，UniFFI 服务 Swift/Kotlin/C#/Python，C-ABI + WASM 服务 TypeScript 和 Web。

**硬性约束**：

| 层 | 职责 | 禁止 |
|---|---|---|
| **核心（C）** | WCCS 全部状态、Fenwick、冷却队列、信用重建、随机映射 | 禁止依赖任何宿主语言运行时 |
| **绑定层** | 类型映射、内存所有权翻译、错误转换、包元数据 | 禁止复制算法逻辑 |
| **随机源** | 由核心通过注入接口调用，绑定层可替换实现 | 绑定层禁止自行消耗随机数 |

> Rust 可以作为核心的**实现语言**，但对外必须导出 `extern "C"` 的 C ABI，而不是依赖 Rust 不稳定的原生 ABI。如果核心直接用 C 写，C++/Rust 绑定就是零成本的头文件包含或 `bindgen` 生成。**本手册以“C 核心 + Rust 辅助绑定生成”为默认路径**，C 核心也可用纯 C99 实现。


## 1. WCCS 核心逻辑规范（语言无关）

这一节是**所有语言必须逐字实现的唯一规格**。绑定层不得偏离。

### 1.1 输入与状态

```text
输入:
  n            : 集合大小 (int32, > 0)
  w[n]         : 基础权重，整数 (uint64)，> 0
  L            : 冷却窗口 (int32, >= 0)
  beta_q       : 软冷却因子，定点 Q32 (uint64)，范围 [0, 2^32]
                 beta_q = round(beta * 2^32)，beta=0 严格冷却，beta=1 无冷却
  alpha_q      : 信用校正强度，定点 Q32 (uint64)，范围 [0, 2^32]
  R            : 信用重建周期 (int32, >= 1)
  seed         : 随机种子 (uint64)
  stream_id    : 并行流编号 (uint64)

状态:
  Fenwick bit[]      : 长度 n+1，Fenwick 树，存储当前动态权重 v_i
  total              : Fenwick 总和 (uint64)
  actual[n]          : 实际抽中次数 (uint64)
  credit[n]          : 信用值，定点 Q32 (int64)
  cooldown[n]        : 冷却因子，定点 Q32 (uint64)，1<<32 表示可用
  recent[L]          : 环形缓冲，存储最近 L 次抽中的索引 (int32)
  recent_head        : 环形缓冲写入位置
  recent_count       : 当前窗口内元素数量
  step               : 总抽样次数 (uint64)，用于信用重建
  rng_state          : 注入的随机源状态（由绑定层管理）
```

**为什么用整数/定点**：跨语言浮点运算（`double` 的舍入、扩展精度、`-ffast-math`）会导致结果不可复现。权重和因子全部用 `uint64` / `int64` 定点，Fenwick 用整数，保证**同 seed 在所有语言、所有平台产出完全相同的索引序列**。Randompack 的跨平台可复现性正是建立在分离引擎与分布、严格控制数值路径的基础上。

### 1.2 动态权重计算

```text
v_i = max(0, w_i + (alpha_q * credit_i) >> 32) * cooldown_i >> 32
```

所有乘法用 `__int128` 或高精度中间类型防止溢出；`>> 32` 是 Q32 定点还原。

### 1.3 无偏随机映射（Lemire）

随机源只提供 `next_u64()`。核心内部必须用无偏映射：

```c
static uint64_t random_below(uint64_t n, wccs_rng_t *rng) {
    uint64_t x = rng->next_u64(rng->ctx);
    __uint128_t m = (__uint128_t)x * n;
    uint64_t l = (uint64_t)m;
    if (l < n) {
        uint64_t t = (0 - n) % n;
        while (l < t) {
            x = rng->next_u64(rng->ctx);
            m = (__uint128_t)x * n;
            l = (uint64_t)m;
        }
    }
    return (uint64_t)(m >> 64);
}
```

**禁止 `% n`**。所有语言的绑定层不得自行生成随机索引，必须走核心的 `wccs_sample`。

### 1.4 采样

```c
// 前置：若 total == 0，清冷却并重建
uint64_t r = random_below(total, rng);          // 在 [0, total) 无偏
int32_t i = fenwick_find_by_cumulative(r);       // Fenwick 前缀和定位

actual[i] += 1;
step += 1;

// 冷却更新
cooldown[i] = beta_q;                             // beta_q=0 严格，beta_q=1<<32 无冷却
fenwick_update(i, calc_v(i));
recent_push(i);                                   // 环形缓冲
if (recent_count == L) {
    int32_t j = recent_pop_oldest();
    cooldown[j] = 1ULL << 32;
    fenwick_update(j, calc_v(j));
}

// 信用重建
if (step % R == 0) rebuild_credit();
return i;
```

### 1.5 信用重建

```text
target_i = (w_i * step) / W_total          // 整数除法，W_total = Σw
credit_i = (int64)(target_i << 32) - (actual_i << 32)
v_i = calc_v(i)
fenwick_rebuild(v)
```

`credit_i` 是“目标次数 - 实际次数”，用 Q32 表示小数部分。`target_i` 用 `uint64` 计算，防溢出。

### 1.6 退化行为（必须逐语言一致）

| 配置 | 行为 |
|---|---|
| `L=0, alpha_q=0` | 纯加权随机，允许重复 |
| `L=1, beta_q=0` | 禁止连续相同 |
| `L≥n, beta_q=0, w` 等权 | 每轮无放回随机排列 |
| `beta_q>0` | 软冷却，高权重不会长期被压死 |
| `total=0` | 清空冷却队列，恢复所有 `cooldown=1<<32`，重建 Fenwick |


## 2. C 核心设计（唯一事实来源）

### 2.1 公共头文件（版本化 ABI）

`include/wccs.h` 是**唯一稳定契约**。用 `cbindgen` 从带注释的 Rust 代码生成，或手写维护。

```c
#define WCCS_ABI_VERSION 1

typedef struct wccs_ctx wccs_ctx;   // 不透明句柄

typedef struct {
    void *ctx;
    uint64_t (*next_u64)(void *ctx);
} wccs_rng_t;

WCCS_API wccs_ctx* wccs_new(
    int32_t n, const uint64_t *w,
    int32_t L, uint64_t beta_q, uint64_t alpha_q,
    int32_t R, uint64_t seed, uint64_t stream_id,
    wccs_rng_t rng, int32_t *err);

WCCS_API int32_t wccs_sample(wccs_ctx *c, int32_t *err);
WCCS_API void wccs_free(wccs_ctx *c);
WCCS_API int32_t wccs_abi_version(void);
```

**ABI 规则**：
- 只追加，不修改已有函数签名和枚举值；
- `err` 使用稳定的错误码（`WCCS_OK=0`, `WCCS_E_INVAL=1`, ...），追加新码不改变旧码数值；
- 不透明句柄 + 构造函数/析构函数 + 方法，不暴露内部结构体布局。

### 2.2 内存所有权

**核心分配的内存，必须由核心释放。** 绑定层不得对核心返回的指针调用宿主语言的 `free`。

```c
// 核心内部用 malloc/free，绑定层通过 wccs_free 释放
// 若核心返回字符串/数组，必须提供配套的 wccs_free_string / wccs_free_array
```

### 2.3 错误处理

**绝不允许 panic/异常穿越 C ABI。** Rust 核心用 `catch_unwind` 包裹每个导出函数，转为错误码。C 核心通过 `err` 输出参数返回。


## 3. 各语言绑定层设计

### 3.1 C / C++

**C**：直接 `#include "wccs.h"`，链接静态库或动态库。这是零成本绑定。

**C++**：在 C ABI 上包一层 RAII。**不引入虚函数、异常、模板到边界层**——边界只做翻译。

```cpp
class Wccs {
    wccs_ctx* c_ = nullptr;
public:
    Wccs(int32_t n, const std::vector<uint64_t>& w, /* ... */) {
        int32_t err = 0;
        c_ = wccs_new(n, w.data(), L, beta_q, alpha_q, R, seed, stream_id, rng_, &err);
        if (err) throw std::runtime_error("wccs init failed");
    }
    int32_t sample() { int32_t e=0; auto r=wccs_sample(c_,&e); if(e) throw ...; return r; }
    ~Wccs() { if (c_) wccs_free(c_); }
};
```

**构建发布**：
- 用 CMake 或 Meson，产出 `libwccs.a` / `libwccs.so|dylib|dll` + 头文件；
- 提供 `pkg-config` 文件或 CMake config；
- Windows 用 `.lib`（导入库）+ `.dll`，导出符号用 `__declspec(dllexport)`。

### 3.2 Rust

Rust 可以有两种角色：

**角色 A：核心实现者**（推荐用于新项目）。`Cargo.toml` 中 `crate-type = ["cdylib", "staticlib"]`，导出 `extern "C"` 函数，用 `cbindgen` 生成 `wccs.h`。

```toml
[lib]
crate-type = ["cdylib", "staticlib"]

[features]
default = []
wasm = ["wasm-bindgen", "getrandom/wasm_js"]
```

**角色 B：消费者**。Rust 侧用 `bindgen` 从 `wccs.h` 生成 FFI 绑定，再包一层 safe API。如果是长期维护的 Rust 优先项目，也可以直接用 `cxx` 桥接 C++，但**不建议**为了“用 Rust”而把核心从 C 搬到 Rust 再搬回来——ABI 层多一跳就多一个失败模式。

**发布**：`cargo build --release` 产出 `.a` / `.so` / `.dylib` / `.dll`。若同时发布到 crates.io，crate 本身是 `rlib`，原生库作为 release artifact 通过 CI 上传到 GitHub Release，下游自行下载链接。

### 3.3 Python

**方案选择**：如果核心是 C ABI，Python 侧优先用 **cffi** 或 **ctypes** 直接调用动态库，不经过 PyO3。这样 Python 绑定与核心解耦，Python 版本升级不需要重编译。

如果坚持用 PyO3（例如希望 Python 侧直接拥有 Rust 核心），用 **maturin** 打包：

```toml
# pyproject.toml
[build-system]
requires = ["maturin>=1.0,<2.0"]
build-backend = "maturin"

[tool.maturin]
features = ["pyo3"]
```

```bash
maturin build --release          # 产出 wheel
maturin publish                  # 发布到 PyPI（Linux 需 manylinux 容器）
```

maturin 实现 PEP 517，支持 CPython 3.8+，可直接上传 PyPI；Linux wheel 必须用 manylinux 或 `cargo-zigbuild` 构建。

**关键**：Python 绑定层**不要做数值运算**，所有 WCCS 逻辑在核心中。Python 只负责构造参数、持有句柄、调用 `wccs_sample`、把错误转为异常。

### 3.4 C#

**首选 P/Invoke 直接调用 C ABI**。这是最稳定、对 Native AOT 最友好的路径。

```csharp
[DllImport("wccs", CallingConvention = CallingConvention.Cdecl)]
static extern IntPtr wccs_new(int n, ulong[] w, int L, ulong beta_q, ulong alpha_q,
    int R, ulong seed, ulong stream_id, ref WccsRng rng, out int err);

[DllImport("wccs", CallingConvention = CallingConvention.Cdecl)]
static extern int wccs_sample(IntPtr ctx, out int err);
```

**Native AOT 注意**：P/Invoke 在 Native AOT 下与非 AOT 无区别，但**未经分析的 P/Invoke 可能在裁剪时出问题**。应对：
- 用 `[LibraryImport]`（.NET 8+ 源生成）替代 `[DllImport]`，编译时绑定；
- 或保留 `[DllImport]` 并显式 `DynamicallyAccessedMembers` / `trimmer` 配置。

**NuGet 打包**：原生库按 RID 放在 `runtimes/{rid}/native/` 下，NuGet 自动选择。`.targets` 文件可选，用于处理传递性原生依赖。**不要**用 `NativeLibrary Include` 静态链接进托管程序集，那会破坏跨平台 NuGet 的 RID 解析。

### 3.5 Node.js

Node.js 的策略取决于运行时范围：

**只服务 Node/Bun**：用 **napi-rs**（Rust）或 **node-addon-api**（C++）构建 `.node` 原生插件。napi-rs 自动生成平台加载器，尝试本地文件 → 可选平台包 → WASI 回退。发布时按平台拆分 optional packages（`@scope/wccs-linux-x64-gnu` 等），根包只含 loader 和元数据。

**需要覆盖 Deno / Edge Workers / 浏览器**：**优先 WASM**。用 `wasm-bindgen` 将 C 核心编译为 `.wasm`，Node 场景可保留原生插件作为高性能路径，非 Node 运行时回退 WASM。

**打包策略**：
- 根包 `main` 指向 napi-rs 生成的 loader，`.node` 文件不要打进 bundle，保持 external；
- 同时生成 CommonJS 和 ESM loader，`exports` 字段显式声明 `import` / `require`；
- WASM 构建用 `--features wasm` 条件编译，排除文件系统依赖。


## 4. 随机源与跨语言可复现性

### 4.1 随机源注入

核心**不内置**随机源，只持有 `wccs_rng_t` 的函数指针。每个绑定层负责提供宿主语言的 RNG：

| 语言 | 默认注入 |
|---|---|
| C/C++ | PCG64 或 Philox（核心附带参考实现） |
| Rust | `rand::rngs::SmallRng` 或 `rand_pcg` |
| Python | `random.Random` 的 `getrandbits(64)` |
| C# | `System.Random` 的 `NextInt64` 或 `RandomNumberGenerator` |
| Node.js | `crypto.randomBytes` 或 `seedrandom` |

**关键约束**：如果目标是**跨语言相同 seed 产出相同序列**，则所有绑定层必须注入**同一个确定性 PRNG**（例如核心附带的 PCG64 参考实现），不能用宿主语言各自的 RNG。Randompack 的可复现性正是通过统一引擎实现，而非依赖平台默认随机源。

### 4.2 可复现的级别

| 级别 | 保证 | 做法 |
|---|---|---|
| L0 | 同语言、同平台可复现 | 固定 seed + 宿主 RNG |
| L1 | 同语言、跨平台可复现 | 整数/定点运算 + 无偏映射 |
| L2 | **跨语言、跨平台可复现** | L1 + 统一注入的确定性 PRNG + 禁止任何浮点差异 |

WCCS 核心规格默认按 L2 设计。如果某个绑定层无法保证（例如 Python 的 `random` 实现差异），文档必须明确降级声明。


## 5. 测试矩阵（必须全绿才能发布）

### 5.1 核心逻辑测试（C 层，唯一权威）

| 测试 | 方法 | 通过标准 |
|---|---|---|
| 频率 | 1e6 次抽样，卡方 | 频率与 `w_i/Σw` 偏差 < ε |
| 冷却违规 | `L=1, beta_q=0` | 连续相同次数 = 0 |
| 覆盖 | `L≥n, 等权` | 每 n 次恰好覆盖全集合 |
| 边界 | `n=1`, `L=0`, `L>n`, `total=0` | 无崩溃、行为符合退化表 |
| 无偏映射 | `n=3,5,7` 各 1e6 次 | 各索引频率均匀 |
| 复现 | 同 seed 两次初始化 | 抽样序列逐位相同 |

### 5.2 跨语言一致性测试

对每个语言绑定，用**同一组固定 seed 和参数**运行 N 次，输出索引序列的 SHA-256，与 C 参考输出比对。

```text
测试向量（必须通过）:
  seed=42, stream_id=0, n=10, w=all 1, L=3, beta_q=0, alpha_q=0, R=1000
  前 100 个输出索引的 SHA-256: <expected>
```

如果某语言无法达到 L2 可复现，该语言测试降级为“同语言内可复现”，但必须在 README 中显著声明。

### 5.3 绑定层边界测试

- 空指针、非法 `n`、`w_i=0`、`beta_q > 2^32` → 返回错误码，不崩溃；
- 核心返回的错误在 Python 是异常、C# 是 `WccsException`、Node 是 `Error`、C++ 是 `std::runtime_error`；
- 并发调用同一句柄（如果线程不安全）必须文档声明，或加锁。

### 5.4 打包验收

| 语言 | 验收 |
|---|---|
| C/C++ | `pkg-config --cflags --libs wccs` 可用；CMake `find_package` 可用 |
| Rust | `cargo test` 通过；`cargo build --release` 产出 `.a`/`.so`/`.dylib`/`.dll` |
| Python | `pip install wccs` 后 `import wccs`；`maturin build` wheel 通过 manylinux 审计 |
| C# | NuGet 包在 `win-x64`, `linux-x64`, `osx-arm64` 上 `dotnet test` 通过；Native AOT 无 trim warning |
| Node.js | Node LTS / Bun / Deno 各跑一次；CJS 和 ESM 都测试 `import` 和 `require` |


## 6. 发布流水线（CI 设计）

### 6.1 构建矩阵

```text
平台: ubuntu-22.04, macos-14, windows-2022
架构: x86_64, aarch64 (macOS/Linux)
语言: C (核心), C++, Rust, Python 3.9–3.13, .NET 8/10, Node 20/22
```

### 6.2 分阶段流水线

**阶段 1：核心构建与测试**（触发所有下游）

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

**阶段 2：各语言绑定构建**（并行）

- Python：`maturin build --release`（Linux 用 manylinux 容器）
- C#：`dotnet pack`，原生库按 RID 放入 `runtimes/`
- Node：`napi build --platform --release`，平台包和根包分别打包
- Rust：`cargo build --release` + `cbindgen` 生成头文件

**阶段 3：跨语言一致性测试**

下载阶段 1 的 C 参考二进制，对每个语言运行固定测试向量，比对 SHA-256。

**阶段 4：发布**

| 目标 | 命令/机制 |
|---|---|
| PyPI | `maturin publish`（或 `uv publish`） |
| NuGet | `dotnet nuget push` |
| npm | 根包 + 平台 optional packages 分别 `npm publish` |
| crates.io | `cargo publish`（如果发布 Rust crate） |
| C/C++ | GitHub Release 附 `.a`/`.so`/`.dylib`/`.dll`/`.h` |

**版本锁定**：所有绑定包的大版本与核心 ABI 版本对齐。`wccs_abi_version()` 返回 `1` 时，所有语言包的 `package.json` / `pyproject.toml` / `.csproj` / `Cargo.toml` 中的 `wccs` 依赖必须解析到 ABI 1 的范围。


## 7. 评审清单（发布前逐项打勾）

- [ ] C ABI 头文件有版本宏，枚举只追加不修改；
- [ ] 所有导出函数有 `catch_unwind`（Rust）或等价错误边界，无 panic 穿越；
- [ ] 核心分配的内存只由 `wccs_free` / `wccs_free_string` 释放，绑定层零 `free`；
- [ ] 所有语言绑定注入的 RNG 满足目标可复现级别，README 明确声明；
- [ ] 无偏映射 `random_below` 在核心中实现，绑定层不自行生成索引；
- [ ] 权重、因子、Fenwick 全部整数/定点，无跨语言浮点分歧；
- [ ] Python wheel 通过 manylinux 审计；
- [ ] C# Native AOT 无 trim/AOT warning；
- [ ] Node 原生插件 external 于 bundle，WASM 回退可加载；
- [ ] 跨语言固定测试向量 SHA-256 一致（或降级声明已文档化）；
- [ ] 冷却、覆盖、边界、频率测试全部通过；
- [ ] 错误码表与异常映射在所有语言中一致。


**一句话总结**：WCCS 多平台库的创作，**70% 的精力在 C 核心与 WCCS 逻辑的确定性，30% 在各语言绑定与打包**。绑定层越薄越好；任何把算法逻辑复制到绑定层的做法，都是跨语言不一致和长期维护负担的源头。