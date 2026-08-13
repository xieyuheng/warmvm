# WarmVM

完全驻留 L1 缓存的虚拟机（28K 宇宙 + 2KB 解释器）。

## 文件

| 文件 | 说明 |
|------|------|
| `wvm.h` | API、控制块布局、.wvm 文件格式 |
| `wvm.c` | 解释器（computed goto，fast-64 跳转表 512B） |
| `warmasm.py` | 汇编器（宿主侧工具） |
| `fib.asm` / `fib.wvm` | 朴素递归 fib 示例（模仿 ~/fibonacci） |
| `bench.c` | 基准：正确性 + 计时 + perf 硬件计数器 |
| `bench/` | 跨语言对比（C/Python/Lua/LuaJIT/Node/Ruby/Java/Guile/Erlang） |

## 构建与运行

```sh
make all           # wvm.exe（计时）、wvmbench.exe（+指令计数）、fib.wvm
./wvm.exe 35       # 计时 fib(35)
./wvmbench.exe counters 35   # L1 缓存缺失测量
./bench/run_all.sh # 跨语言对比
```

## 设计要点

- 28K 宇宙（0x0000–0x6FFF）+ 4K 影子区，宿主数组共 32K
- 控制块 = 0x0000 起的固定 ABI：magic / entry / status / ip / rp / sp / rp0 / sp0
- 参数栈向低地址增长（push: `sp -= 4`），锚在 sp0；返回栈向高地址增长（push: `rp += 2`），锚在 rp0 = 0x0080；共享栈区，相向而遇
- 42 条指令（39 基础 + 3 融合），1 字节 opcode；lit8/lit16/lit32 变长；分支 = op + u16 绝对字节地址
- 融合指令（warmasm.py peephole 自动生成）：`dup lit8 k le`→`duple k`、`lit8 1 sub`→`dec`、`lit8 k sub`→`subk k`
- 所有访问 `& 0x7FFF` 掩码：宿主永不越界（无分支检查）
- `-DWVM_SAFE`：栈碰撞 / 地址越界 / 除零检查

## 实测结果（Meteor Lake，Core Ultra 9 185H）

### L1 驻留（fib(40)，2.35s，21.5 亿条 VM 指令，融合后）

| 事件 | 次数 | 缺失 | 缺失率 |
|------|------|------|--------|
| L1d 读 | 5,080,483,298 | 28 | 0.0000006% |
| L1d 写 | 1,104,013,949 | 5 | 0.0000005% |
| L1i 取指 | 5,074,430,514 | 20 | 0.0000004% |

**约 113 亿次访问仅 53 次缺失**（全部来自上下文切换/启动边界），稳态运行完全驻留 L1。

### 解释器体积

- 代码（L1i）：~2KB（-O2），预算 32K
- 跳转表（L1d）：512B（fast-64，如设计）

### fib(40) 跨语言对比（3 次取最优；以 C -O2 为基准）

| 实现 | 耗时 | 相对 C -O2 |
|------|------|-----------|
| **C (-O2)** | **0.108 s** | **1.0×** |
| Java | 0.297 s | 2.8× |
| Chez Scheme | 0.520 s | 4.8× |
| LuaJIT | 0.540 s | 5.0× |
| C (-O0) | 0.567 s | 5.3× |
| Node.js | 0.729 s | 6.8× |
| Erlang | 0.922 s | 8.5× |
| Guile | 1.579 s | 14.6× |
| **warmvm** | **2.182 s** | **20.2×** |
| Lua 5.4 | 3.831 s | 35.5× |
| Ruby | 5.687 s | 52.7× |
| Python 3.14 | 7.469 s | 69.2× |

（warmvm 一行为指令融合后的结果：fib(40) 步骤 3,146M → 2,153M，-32%；相对 Python 快 3.4×）

warmvm 比 Python 快 2.2×、比 Ruby 快 1.7×，与 Lua 5.4 相当，慢于 JIT/编译型语言 3–33×。

速度：约 **920M VM 指令/秒**（约 4.4 周期/指令，fib(40) 共 21.5 亿条指令）。

### 指令融合效果（warmasm.py peephole）

| | 融合前 | 融合后 |
|---|---|---|
| fib(40) 步骤 | 3,146M | 2,153M（-32%） |
| fib(40) 耗时 | 3.40 s | 2.18 s（**1.56× 提速**） |
| fib(35) 耗时 | 0.31 s | 0.20 s（1.60× 提速） |

融合集：`dup lit8 k le`→`duple k`、`lit8 1 sub`→`dec`、`lit8 k sub`→`subk k`（占用预留的 0x27–0x29 快速槽位，零缓存预算代价）。

### 完整序列 10+20+30+35+40（对照 ~/fibonacci/fibonacci.py，已统一全部含 40）

| 实现 | 耗时 | 相对 C -O2（0.12 s 级） |
|------|------|-----------|
| Python 3.14 | 8.42 s | ~70× |
| **warmvm** | **3.74 s**（0+0.2+28.7+313.6+3396.8 ms） | ~31× |

结论：以 C -O2 为基准（1.0×），warmvm 慢 31.4×，快于 Python（2.2×）与 Ruby（1.7×），与 Lua 5.4 相当；缓存缺失率几乎为零（~160 亿次访问仅 25 次缺失），性能与内存带宽解耦。

## interaction nets 验证（inet.asm / inet-run.c）

在 warmvm 内实现了完整的 interaction nets 引擎：agent 编码（16B 固定块，
端口值 = (base<<4)|port）、活跃对队列（环形 512 项）、规则编译为字节码
（callx 按 (typeA,typeB) 查表分发）、驱动循环（弹出 → 验证防陈旧对 → 分发）。

验证目标：计算 K+K（自然数加法），end 沿结果链计数，最终 counter = 2K。

| K | 正确性 | steps/run | 速度 | L1 缺失 |
|---|--------|-----------|------|---------|
| 100 | 200 ✓ | 99,263 | 972 M/s | 45+45 / 3.48 亿次访问 |
| 250 | 500 ✓ | 246,701 | **1,030 M/s** | 63+63 / 5.18 亿次访问 |

interaction nets 重写负载同样**完全驻留 L1**（缺失率 ~0.00002%），
且吞吐超过 10 亿条 VM 指令/秒。规则/驱动/队列/自由表全部在 VM 内，
宿主只负责搬运内存。

## 多 warmvm 并行验证（inet-par.asm / par-run.c）

两个 warmvm 各占一个线程，完整演示"空间压力 → 导出 → 宿主搬运 → 导入 → 并行归约"：

1. **VM0**（entry=main）：构建 64 叶加法树 T1（agent 区），同时把 T2 直接以**序列化形式**构建在导出缓冲（0x6000，真实的空间压力：两棵树共 384 agents 超出单 VM 容量 276）→ halt(EXPORT)
2. **宿主**：把 3336 字节 bundle（192 agents + 63 活跃对 + 端口重映射信息）memcpy 到 VM1 的导入区
3. **并行阶段**（两线程同时）：VM0 归约 T1，VM1 导入 T2 并归约
4. 结果：count0 + count1 = 64 + 64 = **128 ✓**

```
VM0 build+export: n=192 pairs=63 (0.11 ms)
bundle copied: 3336 bytes
VM0 (reduce T1): count=64  L1d miss=4 L1i miss=4
VM1 (import T2): count=64  L1d miss=6 L1i miss=6
total: 128 (expect 128) OK
```

bundle 格式：`[n][pn][pairs...][records...]`，本地索引 1-based（0 = 无连线）。
导入端：pass-1 分配 + map 重映射，pass-2 端口 remap（`(map[v>>4-1]<<4)|(v&15)`），
pairs 入队 → 驱动循环。规则/驱动/队列/自由表全部在 VM 内，宿主只做一次 memcpy。

## inet-lisp 语法前端（inet-c/）

把 inet-lisp（projects/xieyuheng/inet-lisp）的解析层整体复制过来，写了一个新的后端编译器：

```
.lisp ──► inet-c/ (复制的 lexer/sexp/parser) ──► compile_wvm.c ──► .asm ──► warmasm.py ──► .wvm
```

- **wvmc.exe**：AST → warmvm 汇编（两遍，复用 warmasm.py）
- 语义映射：agent = 16B 块 [type,p0,p1,p2]，端口值 (base<<4)|port（编码端口 0=principal，
  任意声明位置），TABLE[(F<<4)|G] 双向填表，规则 handler 按类型 swap 规范化
- 支持：define-node / define-rule / define（0 参函数）/ define-function（warmvm 子程序，
  栈传参）、规则体 connect / 节点应用 / (= x y (f ...)) 赋值
- v1 限制：无 import/define-rule*/数字字面量/apply-wire（报错提示）
- 内存布局：QUEUE 0x4024 / TABLE 0x4824 / SCR 0x4A24 / CSLOT 0x4A4C / VARS 0x4A5C /
  AGENTS 0x4C00（105 个变量槽）

验证：
```
$ make demo-nat   # (add (one) (two)) → add1 count = 3 OK
$ make demo-mul   # (mul (two) (three)) → add1 count = 6 OK  (6 条规则)
$ perf: 2000 次完整归约 10.6 us/run, L1d/L1i miss 各 2 次 (总数)
```

调试中修掉的编译器 bug（每类都对应一个语义陷阱）：
- principal 端口可出现在任意声明位置 → 编码端口重排 (enc[])
- 模式变量绑定的值是"线的对端"，端口位运行时才可知 → 运行时 enq 检查
- apply 参数数 = arity 时仍要 alloc+连接（不能只求值参数）
- 变量槽与引擎临时槽 (SCR+36) 重叠 → 布局隔离
- 函数嵌套调用 → 槽递增分配不复用
