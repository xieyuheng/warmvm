; ============================================================
; interaction nets 并行验证: 平衡二叉树加法 + bundle 迁移
;
; 任务: 两个 64 叶加法树 (T1, T2)。单 VM 放不下两棵树:
;   T1 = 192 agents (在 agent 区), T2 = 192 agents (直接在导出缓冲
;   里以序列化形式构建 —— 真实的空间压力)。
;
; 流程:
;   main:        初始化 → 构建 T1 → 构建 T2 导出 → halt(err=EXPORT)
;   reduce_entry: 驱动循环归约 T1 (宿主在导出后重新进入)
;   import_entry: 导入 T2 (alloc + 指针重映射 + 入队) → 驱动循环
;
; bundle 格式 (XBASE=0x6000):
;   [0] u32 n (agent 数)  [4] u32 pn (活跃对数)
;   [8..]   pair 表 (local idx, 最多 64 项)
;   [0x108 + i*16]  agent 记录: type, p0, p1, p2 (local 坐标)
; ============================================================
.eq FREE   0x4000
.eq QHEAD  0x4004
.eq QTAIL  0x4008
.eq COUNT  0x400C
.eq ERR    0x4010
.eq PEAK   0x4014
.eq XCNT   0x4018
.eq QUEUE  0x4024
.eq TABLE  0x4824
.eq SLOT   0x4A28   ; 树构建槽 (按深度: SLOT + d*16)
.eq CSLOT  0x4AA0   ; connect 专用
.eq SCR    0x4AC0   ; 规则/驱动临时槽
.eq AGENTS 0x4B00
.eq MAP    0x5C00   ; 导入映射 (local idx -> 真实地址)
.eq XBASE  0x6000   ; 导出/导入缓冲
.eq XREC   0x6108   ; bundle 记录区起点
.eq DEPTH  6        ; 2^6 = 64 叶
; 规则表地址 (TABLE + key*2)
.eq T13 0x484A
.eq T31 0x4886
.eq T23 0x486A
.eq T32 0x4888
.eq T14 0x484C
.eq T41 0x48A6
.eq T24 0x486C
.eq T42 0x48A8

.sp0 0x1000
.entry main

; ---------------- 基础子程序 ----------------

; pc_addr: [端口值 v] -> [端口 cell 字节地址]
pc_addr:
    dup
    lit8 4
    shr
    swap
    lit8 15
    and
    lit8 2
    shl
    add
    lit8 4
    add
    ret

; pc_addr_x: 导出空间版 (记录在 XREC + idx*16)
pc_addr_x:
    dup
    lit8 4
    shr
    lit8 1
    sub
    lit8 4
    shl
    swap
    lit8 15
    and
    lit8 2
    shl
    add
    lit8 4
    add
    lit16 XREC
    add
    ret

; portv: [agent, port] -> [(agent<<4)|port]
portv:
    swap
    lit8 4
    shl
    swap
    or
    ret

; connect: [v1 v2] -> 双写 (agent 区)
connect:
    swap
    lit16 CSLOT
    store
    lit16 CSLOT+4
    store
    lit16 CSLOT
    load
    dup
    call pc_addr
    lit16 CSLOT+8
    store
    lit16 CSLOT+4
    load
    dup
    call pc_addr
    lit16 CSLOT+12
    store
    lit16 CSLOT+4
    load
    lit16 CSLOT+8
    load
    store
    lit16 CSLOT
    load
    lit16 CSLOT+12
    load
    store
    drop
    drop
    ret

; connect_x: 导出空间版
connect_x:
    swap
    lit16 CSLOT
    store
    lit16 CSLOT+4
    store
    lit16 CSLOT
    load
    dup
    call pc_addr_x
    lit16 CSLOT+8
    store
    lit16 CSLOT+4
    load
    dup
    call pc_addr_x
    lit16 CSLOT+12
    store
    lit16 CSLOT+4
    load
    lit16 CSLOT+8
    load
    store
    lit16 CSLOT
    load
    lit16 CSLOT+12
    load
    store
    drop
    drop
    ret

; newag: [type] -> [addr]
newag:
    lit16 SCR+32
    store
    lit16 FREE
    load
    dup
    jnz .nw_ok
    drop
    lit16 2
    lit16 ERR
    store
    halt
.nw_ok:
    lit16 SCR+36
    store
    lit16 SCR+36
    load
    load
    lit16 FREE
    store
    lit16 SCR+32
    load
    lit16 SCR+36
    load
    store
    lit16 SCR+36
    load
    ret

; freeag: [addr]
freeag:
    lit16 SCR+36
    store
    lit16 FREE
    load
    lit16 SCR+36
    load
    store
    lit16 SCR+36
    load
    lit16 FREE
    store
    ret

; enq: [agent_addr] (环形队列, 带峰值跟踪)
enq:
    lit16 QTAIL
    load
    dup
    lit16 QHEAD
    load
    sub
    lit16 PEAK
    load
    lt
    jnz .no_peak
    dup
    lit16 QHEAD
    load
    sub
    lit16 PEAK
    store
.no_peak:
    dup
    lit16 511
    and
    lit8 2
    shl
    lit16 QUEUE
    add
    rot
    swap
    store
    lit8 1
    add
    lit16 QTAIL
    store
    ret

; xalloc: [type] -> [local_idx+1]  (1-based, 0 = 无连线)
xalloc:
    lit16 XCNT
    load
    dup
    lit8 4
    shl
    lit16 XREC
    add
    swap
    drop
    store              ; mem[XREC + cnt*16] = type
    lit16 XCNT
    load
    lit8 1
    add
    lit16 XCNT
    store
    lit16 XCNT
    load
    ret

; ---------------- 树构建 ----------------

; bt_slot: [d] -> [SLOT + d*16]  (消费 d)
bt_slot:
    lit8 4
    shl
    lit16 SLOT
    add
    ret

; child_iface: [child] -> [child 的接口端口值]
;   add (type 3) → 端口 2 (result)；add1 叶子 (type 2) → 端口 0 (principal)
child_iface:
    dup
    load
    lit8 2
    eq
    jnz .ci_add1
    lit8 2
    call portv
    ret
.ci_add1:
    lit8 0
    call portv
    ret

; build_tree: [depth] -> [head]  递归构建加法树 (agent 区, 按深度槽位)
build_tree:
    dup
    jz .bt_leaf
    dup
    dup
    call bt_slot
    store              ; mem[slot(d)] = d ; [d]
    dup
    lit8 1
    sub
    call build_tree    ; [d l]
    over
    call bt_slot
    lit8 4
    add
    store              ; mem[slot+4] = l ; [d]
    dup
    lit8 1
    sub
    call build_tree    ; [d r]
    over
    call bt_slot
    lit8 8
    add
    store              ; mem[slot+8] = r ; [d]
    lit8 3
    call newag         ; [d add]
    over
    call bt_slot
    lit8 12
    add
    store              ; mem[slot+12] = add ; [d]
    ; connect(l 接口, add.p0)
    dup
    call bt_slot
    lit8 4
    add
    load               ; [d l]
    over
    call bt_slot
    lit8 12
    add
    load               ; [d l add]
    lit8 0
    call portv         ; [d l v_add]
    over
    call child_iface   ; [d l v_add v_l]
    call connect       ; [d l]
    drop               ; [d]
    ; connect(r 接口, add.p1)
    dup
    call bt_slot
    lit8 8
    add
    load               ; [d r]
    over
    call bt_slot
    lit8 12
    add
    load               ; [d r add]
    lit8 1
    call portv         ; [d r v_add]
    over
    call child_iface   ; [d r v_add v_r]
    call connect       ; [d r]
    drop               ; [d]
; enq(add)
    dup
    call bt_slot
    lit8 12
    add
    load
    call enq           ; [d]
    ; 返回 add (栈顶是本层深度)
    dup
    call bt_slot
    lit8 12
    add
    load
    swap
    drop
    ret
.bt_leaf:
    drop
    lit8 1
    call newag
    lit8 2
    call newag
    lit16 SLOT+12
    store              ; mem[SLOT+12] = a (add1) ; [z]
    dup
    lit8 0
    call portv         ; [z v_z]
    lit16 SLOT+12
    load
    lit8 1
    call portv         ; [z v_z v_a]
    call connect       ; [z]
    drop
    lit16 SLOT+12
    load               ; [a]
    ret

; child_iface_x: [child] -> [接口端口值]  (导出空间: 类型在 XREC + child*16)
child_iface_x:
    dup
    lit8 4
    shl
    lit16 XREC
    add
    lit16 16
    sub
    load
    lit8 2
    eq
    jnz .cix_add1
    lit8 2
    call portv
    ret
.cix_add1:
    lit8 0
    call portv
    ret

; build_tree_x: [depth] -> [local_idx]  直接在导出缓冲构建
build_tree_x:
    dup
    jz .bx_leaf
    dup
    dup
    call bt_slot
    store              ; [d]
    dup
    lit8 1
    sub
    call build_tree_x  ; [d l]
    over
    call bt_slot
    lit8 4
    add
    store              ; [d]
    dup
    lit8 1
    sub
    call build_tree_x  ; [d r]
    over
    call bt_slot
    lit8 8
    add
    store              ; [d]
    lit8 3
    call xalloc        ; [d add]
    over
    call bt_slot
    lit8 12
    add
    store              ; [d]
    ; connect_x(l 接口, add.p0)
    dup
    call bt_slot
    lit8 4
    add
    load               ; [d l]
    over
    call bt_slot
    lit8 12
    add
    load               ; [d l add]
    lit8 0
    call portv         ; [d l v_add]
    over
    call child_iface_x ; [d l v_add v_l]
    call connect_x       ; [d l]
    drop               ; [d]
    ; connect_x(r 接口, add.p1)
    dup
    call bt_slot
    lit8 8
    add
    load               ; [d r]
    over
    call bt_slot
    lit8 12
    add
    load               ; [d r add]
    lit8 1
    call portv         ; [d r v_add]
    over
    call child_iface_x ; [d r v_add v_r]
    call connect_x       ; [d r]
    drop               ; [d]
; 导出活跃对
    dup
    call bt_slot
    lit8 12
    add
    load               ; [d add]
    lit16 XBASE+4
    load               ; [d add pn]
    dup
    lit8 2
    shl
    lit16 XBASE+8
    add                ; [d add pn paddr]
    rot                ; [d pn paddr add]
    swap               ; [d pn add paddr]
    store              ; mem[paddr] = add ; [d pn]
    lit8 1
    add
    lit16 XBASE+4
    store              ; pn++ ; [d]
    ; 返回 add (栈顶是本层深度)
    dup
    call bt_slot
    lit8 12
    add
    load
    swap
    drop
    ret
.bx_leaf:
    drop
    lit8 1
    call xalloc
    lit8 2
    call xalloc
    lit16 SLOT+12
    store              ; mem[SLOT+12] = a ; [z]
    dup
    lit8 0
    call portv
    lit16 SLOT+12
    load
    lit8 1
    call portv
    call connect_x
    drop
    lit16 SLOT+12
    load
    ret

; ---------------- 规则 ----------------

; (add, zero)
rule_add_zero:
    dup
    load
    lit8 3
    eq
    jnz .az_ok
    swap
.az_ok:
    lit16 SCR
    store
    lit16 SCR+4
    store
    lit16 SCR
    load
    lit16 8
    add
    load
    lit16 SCR
    load
    lit16 12
    add
    load
    dup
    lit8 15
    and
    jnz .az_noy
    swap
    dup
    lit8 15
    and
    jnz .az_noy2
    dup
    lit8 4
    shr
    call enq
.az_noy2:
    swap
.az_noy:
    call connect
    lit16 SCR
    load
    call freeag
    lit16 SCR+4
    load
    call freeag
    ret

; (add, add1)
rule_add_add1:
    dup
    load
    lit8 3
    eq
    jnz .aa_ok
    swap
.aa_ok:
    lit16 SCR
    store
    lit16 SCR+4
    store
    lit16 SCR+4
    load
    lit16 8
    add
    load
    lit16 SCR+8
    store
    lit16 SCR
    load
    lit16 8
    add
    load
    lit16 SCR+12
    store
    lit16 SCR
    load
    lit16 12
    add
    load
    lit16 SCR+16
    store
    lit8 2
    call newag
    lit16 SCR+20
    store
    lit8 3
    call newag
    lit16 SCR+24
    store
    ; 1a. mem[pc_addr(y)] = C<<4
    lit16 SCR+16
    load
    dup
    call pc_addr
    lit16 SCR+20
    load
    lit8 4
    shl
    swap
    store
    drop
    ; 1b. mem[C+4] = y
    lit16 SCR+16
    load
    lit16 SCR+20
    load
    lit16 4
    add
    store
    ; 2a. mem[pc_addr(z)] = D<<4
    lit16 SCR+8
    load
    dup
    call pc_addr
    lit16 SCR+24
    load
    lit8 4
    shl
    swap
    store
    drop
    ; 2b. mem[D+4] = z
    lit16 SCR+8
    load
    lit16 SCR+24
    load
    lit16 4
    add
    store
    ; 3a. mem[pc_addr(x)] = (D<<4)|1
    lit16 SCR+12
    load
    dup
    call pc_addr
    lit16 SCR+24
    load
    lit8 4
    shl
    lit8 1
    or
    swap
    store
    drop
    ; 3b. mem[D+8] = x
    lit16 SCR+12
    load
    lit16 SCR+24
    load
    lit16 8
    add
    store
    ; 4a. mem[D+12] = (C<<4)|1  (D.result <-> C.prev)
    lit16 SCR+20
    load
    lit8 4
    shl
    lit8 1
    or
    lit16 SCR+24
    load
    lit16 12
    add
    store
    ; 4b. mem[C+8] = (D<<4)|2
    lit16 SCR+24
    load
    lit8 4
    shl
    lit8 2
    or
    lit16 SCR+20
    load
    lit16 8
    add
    store
    ; 新活跃对: y 主 -> enq(y>>4)
    lit16 SCR+16
    load
    dup
    lit8 15
    and
    jnz .aa_noy
    dup
    lit8 4
    shr
    call enq
    drop
    jmp .aa_yz
.aa_noy:
    drop
.aa_yz:
    ; z 主 -> enq(z>>4)
    lit16 SCR+8
    load
    dup
    lit8 15
    and
    jnz .aa_noz
    dup
    lit8 4
    shr
    call enq
    drop
    jmp .aa_done
.aa_noz:
    drop
.aa_done:
    lit16 SCR
    load
    call freeag
    lit16 SCR+4
    load
    call freeag
    ret

; (end, add1)
rule_end_add1:
    dup
    load
    lit8 4
    eq
    jnz .ea_ok
    swap
.ea_ok:
    lit16 SCR
    store
    lit16 SCR+4
    store
    lit16 COUNT
    load
    lit8 1
    add
    lit16 COUNT
    store
    lit16 SCR+4
    load
    lit16 8
    add
    load
    dup
    lit8 15
    and
    jnz .ea_nopair
    dup
    lit8 4
    shr
    call enq
    drop
    jmp .ea_pairdone
.ea_nopair:
    drop
.ea_pairdone:
    lit16 SCR+4
    load
    lit16 8
    add
    load
    dup
    call pc_addr
    lit16 SCR
    load
    lit8 4
    shl
    swap
    store
    drop
    lit16 SCR+4
    load
    lit16 8
    add
    load
    lit16 SCR
    load
    lit16 4
    add
    store
    lit16 SCR+4
    load
    call freeag
    ret

; (end, zero)
rule_end_zero:
    dup
    load
    lit8 4
    eq
    jnz .ez_ok
    swap
.ez_ok:
    lit16 SCR
    store
    lit16 SCR+4
    store
    lit16 SCR
    load
    call freeag
    lit16 SCR+4
    load
    call freeag
    ret

; ---------------- 驱动循环 ----------------

.driver:
    lit16 QHEAD
    load
    lit16 QTAIL
    load
    dup
    rot
    eq
    jnz .done
    drop
    ; 弹出 a (环形)
    lit16 QHEAD
    load
    dup
    lit16 511
    and
    lit8 2
    shl
    lit16 QUEUE
    add
    load
    swap
    lit8 1
    add
    lit16 QHEAD
    store
    ; typeA 有效
    dup
    load
    dup
    jnz .ta_ok
    drop
    jmp .drop
.ta_ok:
    lit8 5
    lt
    jz .drop
    ; B = mem[a+4]>>4
    dup
    lit16 4
    add
    load
    lit8 4
    shr
    swap
    lit16 SCR
    store
    lit16 SCR+4
    store
    ; back 检查
    lit16 SCR
    load
    lit8 4
    shl
    lit16 SCR+4
    load
    lit16 4
    add
    load
    eq
    jz .drop
    ; typeB 有效
    lit16 SCR+4
    load
    load
    dup
    jnz .tb_ok
    drop
    jmp .drop
.tb_ok:
    lit8 5
    lt
    jz .drop
    ; 分发
    lit16 SCR
    load
    load
    lit8 4
    shl
    lit16 SCR+4
    load
    load
    or
    dup
    lit8 1
    shl
    lit16 TABLE
    add
    load16
    dup
    jnz .has_rule
    drop
    jmp .norule
.has_rule:
    swap
    drop
    lit16 SCR
    load
    lit16 SCR+4
    load
    rot
    callx
    jmp .driver
.drop:
    jmp .driver
.norule:
    lit16 3
    lit16 ERR
    store
    halt
.done:
    halt

; ---------------- 入口 ----------------

main:
    lit16 0
    lit16 FREE
    store
    lit16 0
    lit16 QHEAD
    store
    lit16 0
    lit16 QTAIL
    store
    lit16 0
    lit16 COUNT
    store
    lit16 0
    lit16 ERR
    store
    lit16 0
    lit16 XCNT
    store
    ; 自由表: 0x4AC0..0x5C00
    lit16 0x5C00
    lit16 SLOT+36
    store
    lit16 AGENTS
.init_loop:
    dup
    lit16 16
    add
    dup
    lit16 SLOT+36
    load
    lt
    jz .init_last
    dup
    rot
    store
    jmp .init_loop
.init_last:
    drop
    lit16 0
    swap
    store
    lit16 AGENTS
    lit16 FREE
    store
    ; 规则表
    lit16 rule_add_zero
    lit16 T13
    store16
    lit16 rule_add_zero
    lit16 T31
    store16
    lit16 rule_add_add1
    lit16 T23
    store16
    lit16 rule_add_add1
    lit16 T32
    store16
    lit16 rule_end_zero
    lit16 T14
    store16
    lit16 rule_end_zero
    lit16 T41
    store16
    lit16 rule_end_add1
    lit16 T24
    store16
    lit16 rule_end_add1
    lit16 T42
    store16
    ; 构建 T1
    lit16 DEPTH
    call build_tree
    lit16 SLOT+40
    store
    lit8 4
    call newag
    lit16 SLOT+44
    store
    ; root1.p2 <-> end1.p0
    lit16 SLOT+40
    load
    lit8 2
    call portv
    lit16 SLOT+44
    load
    lit8 0
    call portv
    call connect
    ; 构建 T2 导出
    lit16 DEPTH
    call build_tree_x
    lit16 SLOT+48
    store
    lit8 4
    call xalloc
    lit16 SLOT+52
    store
    lit16 SLOT+48
    load
    lit8 2
    call portv
    lit16 SLOT+52
    load
    lit8 0
    call portv
    call connect_x
    ; 头部: n = XCNT
    lit16 XCNT
    load
    lit16 XBASE
    store
    ; err = 1 (EXPORT 就绪)
    lit8 1
    lit16 ERR
    store
    halt

reduce_entry:
    jmp .driver

import_entry:
    ; 初始化 (自由表 + 全局)
    lit16 0
    lit16 FREE
    store
    lit16 0
    lit16 QHEAD
    store
    lit16 0
    lit16 QTAIL
    store
    lit16 0
    lit16 COUNT
    store
    lit16 0
    lit16 ERR
    store
    lit16 0x5C00
    lit16 SLOT+36
    store
    lit16 AGENTS
.imp_init_loop:
    dup
    lit16 16
    add
    dup
    lit16 SLOT+36
    load
    lt
    jz .imp_init_last
    dup
    rot
    store
    jmp .imp_init_loop
.imp_init_last:
    drop
    lit16 0
    swap
    store
    lit16 AGENTS
    lit16 FREE
    store
    ; 规则表
    lit16 rule_add_zero
    lit16 T13
    store16
    lit16 rule_add_zero
    lit16 T31
    store16
    lit16 rule_add_add1
    lit16 T23
    store16
    lit16 rule_add_add1
    lit16 T32
    store16
    lit16 rule_end_zero
    lit16 T14
    store16
    lit16 rule_end_zero
    lit16 T41
    store16
    lit16 rule_end_add1
    lit16 T24
    store16
    lit16 rule_end_add1
    lit16 T42
    store16
    ; n = mem[XBASE], pn = mem[XBASE+4]
    lit16 XBASE
    load
    lit16 SLOT+24
    store
    lit16 XBASE+4
    load
    lit16 SLOT+28
    store
    ; pass 1: 分配 + 映射
    lit16 0
.imp1:
    dup
    lit16 SLOT+24
    load
    lt
    jz .imp2
    dup
    lit8 4
    shl
    lit16 XREC
    add
    load
    call newag
    over
    lit8 2
    shl
    lit16 MAP
    add
    store
    lit8 1
    add
    jmp .imp1
.imp2:
    drop
    ; pass 2: 重映射端口 (3 个端口手动展开)
    lit16 0
.imp3:
    dup
    lit16 SLOT+24
    load
    lt
    jz .imp_done3
    lit16 SLOT+36
    store              ; SLOT+36 = i ; []
    ; ai = map[i]
    lit16 SLOT+36
    load
    lit8 2
    shl
    lit16 MAP
    add
    load
    lit16 SLOT+32
    store              ; SLOT+32 = ai
    ; p0
    lit16 SLOT+36
    load
    lit8 4
    shl
    lit16 XREC
    add
    lit16 4
    add
    load
    dup
    jz .p0_skip
    dup
    lit8 4
    shr
    lit8 1
    sub
    lit8 2
    shl
    lit16 MAP
    add
    load
    lit8 4
    shl
    swap
    lit8 15
    and
    or
    lit16 SLOT+32
    load
    lit16 4
    add
    store
    jmp .p0_done
.p0_skip:
    drop
.p0_done:
    ; p1
    lit16 SLOT+36
    load
    lit8 4
    shl
    lit16 XREC
    add
    lit16 8
    add
    load
    dup
    jz .p1_skip
    dup
    lit8 4
    shr
    lit8 1
    sub
    lit8 2
    shl
    lit16 MAP
    add
    load
    lit8 4
    shl
    swap
    lit8 15
    and
    or
    lit16 SLOT+32
    load
    lit16 8
    add
    store
    jmp .p1_done
.p1_skip:
    drop
.p1_done:
    ; p2
    lit16 SLOT+36
    load
    lit8 4
    shl
    lit16 XREC
    add
    lit16 12
    add
    load
    dup
    jz .p2_skip
    dup
    lit8 4
    shr
    lit8 1
    sub
    lit8 2
    shl
    lit16 MAP
    add
    load
    lit8 4
    shl
    swap
    lit8 15
    and
    or
    lit16 SLOT+32
    load
    lit16 12
    add
    store
    jmp .p2_done
.p2_skip:
    drop
.p2_done:
    lit16 SLOT+36
    load
    lit8 1
    add
    jmp .imp3
.imp_done3:
    drop
    ; 活跃对入队
    lit16 0
.imp4:
    dup
    lit16 SLOT+28
    load
    lt
    jz .imp_done4
    dup
    lit8 2
    shl
    lit16 XBASE+8
    add
    load
    lit8 1
    sub
    lit8 2
    shl
    lit16 MAP
    add
    load
    call enq
    lit8 1
    add
    jmp .imp4
.imp_done4:
    drop
    jmp .driver
