; ============================================================
; interaction nets 验证程序（warmvm 内完整引擎）
;
; agent 类型: 1=zero  2=add1  3=add  4=end
; agent 块: 16B = [type, p0, p1, p2]（固定块，端口多余时浪费）
; 端口值: (agent_base << 4) | port_index；线 = 两个端口 cell 互指
; 活跃对队列: u32 agent 地址数组；驱动循环弹出并验证（防陈旧对）
;
; 验证目标: 计算 K+K，end 沿结果链走一遍计数，最终 counter = 2K
; ============================================================
.eq FREE   0x4000
.eq QHEAD  0x4004
.eq QTAIL  0x4008
.eq COUNT  0x400C
.eq ERR    0x4010
.eq PEAK   0x4014
.eq FA     0x4014
.eq FB     0x4018
.eq FC     0x401C
.eq FD     0x4020
.eq QUEUE  0x4024
.eq TABLE  0x4824
.eq SLOT   0x4A24
.eq CSLOT  0x4A50   ; connect 专用临时槽 (v1 v2 a1 a2)
.eq SCR    0x4A70
.eq AGENTS 0x4AC0
.eq K      250
; 规则表条目（TABLE + key*2, key = typeA<<4|typeB）
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

; ---------------- 辅助子程序 ----------------

; pc_addr: [端口值 v] -> [端口 cell 的字节地址]
;   addr = (v >> 4) + ((v & 15) << 2) + 4
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

; portv: [agent_addr, port] -> [(addr<<4)|port]
portv:
    swap
    lit8 4
    shl
    swap
    or
    ret

; connect: [v1, v2] -> 两端口互指
;   mem[pc_addr(v1)] = v2; mem[pc_addr(v2)] = v1
connect:
    ; [v1 v2] -> mem[pc_addr(v1)] = v2; mem[pc_addr(v2)] = v1
    ; 专用槽位 CSLOT+0=v1, +4=v2, +8=a1, +12=a2（不与其他代码共享）
    swap
    lit16 CSLOT
    store              ; CSLOT+0 = v1
    lit16 CSLOT+4
    store              ; CSLOT+4 = v2
    lit16 CSLOT
    load
    dup
    call pc_addr
    lit16 CSLOT+8
    store              ; CSLOT+8 = a1
    lit16 CSLOT+4
    load
    dup
    call pc_addr
    lit16 CSLOT+12
    store              ; CSLOT+12 = a2
    lit16 CSLOT+4
    load
    lit16 CSLOT+8
    load
    store              ; mem[a1] = v2
    lit16 CSLOT
    load
    lit16 CSLOT+12
    load
    store              ; mem[a2] = v1
    drop
    drop               ; 消耗入口参数副本
    ret

; newag: [type] -> [addr]  从自由表取一个 agent
newag:
    lit16 SCR+32
    store              ; SCR+32 = type
    lit16 FREE
    load               ; [h]
    dup
    jnz .nw_ok
    drop
    lit16 2
    lit16 ERR
    store              ; ERR = 2 (OOM)
    halt
.nw_ok:
    lit16 SCR+36
    store              ; SCR+36 = h (新 agent 地址)
    lit16 SCR+36
    load
    load               ; [h'] = mem[h] (自由表下一个)
    lit16 FREE
    store              ; mem[FREE] = h'
    lit16 SCR+32
    load               ; [type]
    lit16 SCR+36
    load               ; [type h]
    store              ; mem[h] = type
    lit16 SCR+36
    load               ; [h]
    ret

; freeag: [addr] -> 释放 agent 到自由表
freeag:
    lit16 SCR+36
    store              ; SCR+36 = addr
    lit16 FREE
    load               ; [h]
    lit16 SCR+36
    load               ; [h addr]
    store              ; mem[addr] = h
    lit16 SCR+36
    load               ; [addr]
    lit16 FREE
    store              ; mem[FREE] = addr
    ret

; enq: [agent_addr] -> 加入活跃对队列
enq:
    lit16 QTAIL
    load               ; [addr t]
    ; 峰值跟踪: PEAK = max(PEAK, t - QHEAD)
    dup
    lit16 QHEAD
    load
    sub                ; [addr t len]
    lit16 PEAK
    load               ; [addr t len pk]
    lt                 ; (len < pk) ; [addr t flag]
    jnz .no_peak       ; [addr t]
    dup
    lit16 QHEAD
    load
    sub                ; [addr t len]
    lit16 PEAK
    store              ; PEAK = len ; [addr t]
.no_peak:              ; [addr t]
    dup
    lit16 511
    and
    lit8 2
    shl
    lit16 QUEUE
    add                ; [addr t qaddr]
    rot                ; [t qaddr addr]
    swap               ; [t addr qaddr]
    store              ; mem[qaddr] = addr ; [t]
    lit8 1
    add
    lit16 QTAIL
    store              ; q_tail++
    ret

; build_chain: [k] -> [链头 add1 地址]
;   构建 k 个 add1 + 1 个 zero 的链；返回最后一个 add1（链头）
build_chain:
    lit8 1
    call newag         ; [k z]
    lit16 CSLOT+16
    store              ; CSLOT+16 = prev = zero ; [k]
.chain_loop:
    dup
    jz .chain_done     ; [k]
    lit8 2
    call newag         ; [k a]
    ; conn(portv(prev,0), portv(a,1))
    lit16 CSLOT+16
    load
    lit8 0
    call portv         ; [k a v1]
    over               ; [k a v1 a]
    lit8 1
    call portv         ; [k a v1 v2]
    call connect       ; [k a]
    lit16 CSLOT+16
    store              ; prev = a ; [k]
    lit8 1
    sub                ; [k-1]
    jmp .chain_loop
.chain_done:           ; [0]
    drop               ; []
    lit16 CSLOT+16
    load               ; [head]
    ret

; ---------------- 规则 ----------------

; (add, zero): a=add b=zero
;   将 addend 线与 result 线连通；释放 a b
;   若两端都是主端口则产生新活跃对
rule_add_zero:
    dup
    load
    lit8 3
    eq
    jnz .az_ok
    swap
.az_ok:
    lit16 SCR
    store              ; SCR = a
    lit16 SCR+4
    store              ; SCR+4 = b
    ; x = mem[a+8] (addend 对端), y = mem[a+12] (result 对端)
    lit16 SCR
    load
    lit16 8
    add
    load               ; [x]
    lit16 SCR
    load
    lit16 12
    add
    load               ; [x y]
    ; 新活跃对: x,y 都是主端口?
    dup
    lit8 15
    and
    jnz .az_noy           ; [x y]
    swap               ; [y x]
    dup
    lit8 15
    and
    jnz .az_noy2          ; [y x]
    dup
    lit8 4
    shr                ; [y x xa]
    call enq           ; [y x]
.az_noy2:
    swap               ; [x y]
.az_noy:                  ; [x y]
    call connect
    lit16 SCR
    load
    call freeag
    lit16 SCR+4
    load
    call freeag
    ret

; (add, add1): a=add b=add1
;   prev 线 -> 新 add 的 target!；addend 线 -> 新 add 的 addend；
;   result 线 -> 新 add1 的 value!；新 add1.prev <-> 新 add.result
rule_add_add1:
    dup
    load
    lit8 3
    eq
    jnz .aa_ok
    swap
.aa_ok:
    lit16 SCR
    store              ; SCR = a (add)
    lit16 SCR+4
    store              ; SCR+4 = b (add1)
    lit16 SCR+4
    load
    lit16 8
    add
    load               ; [z]  (prev 线对端)
    lit16 SCR+8
    store              ; SCR+8 = z
    lit16 SCR
    load
    lit16 8
    add
    load               ; [x]  (addend 线对端)
    lit16 SCR+12
    store              ; SCR+12 = x
    lit16 SCR
    load
    lit16 12
    add
    load               ; [y]  (result 线对端)
    lit16 SCR+16
    store              ; SCR+16 = y
    lit8 2
    call newag
    lit16 SCR+20
    store              ; SCR+20 = C (新 add1)
    lit8 3
    call newag
    lit16 SCR+24
    store              ; SCR+24 = D (新 add)
    ; 1a. mem[pc_addr(y)] = C<<4
    lit16 SCR+16
    load
    dup
    call pc_addr       ; [y ay]
    lit16 SCR+20
    load
    lit8 4
    shl                ; [y ay C4]
    swap               ; [y C4 ay]
    store              ; mem[ay] = C4 ; [y]
    drop
    ; 1b. mem[C+4] = y
    lit16 SCR+16
    load
    lit16 SCR+20
    load
    lit16 4
    add
    store              ; mem[C+4] = y
    ; 2a. mem[pc_addr(z)] = D<<4
    lit16 SCR+8
    load
    dup
    call pc_addr       ; [z az]
    lit16 SCR+24
    load
    lit8 4
    shl                ; [z az D4]
    swap               ; [z D4 az]
    store              ; mem[az] = D4 ; [z]
    drop
    ; 2b. mem[D+4] = z
    lit16 SCR+8
    load
    lit16 SCR+24
    load
    lit16 4
    add
    store              ; mem[D+4] = z
    ; 3a. mem[pc_addr(x)] = (D<<4)|1
    lit16 SCR+12
    load
    dup
    call pc_addr       ; [x ax]
    lit16 SCR+24
    load
    lit8 4
    shl
    lit8 1
    or                 ; [x ax D1]
    swap               ; [x D1 ax]
    store              ; mem[ax] = D1 ; [x]
    drop
    ; 3b. mem[D+8] = x
    lit16 SCR+12
    load
    lit16 SCR+24
    load
    lit16 8
    add
    store              ; mem[D+8] = x
    ; 4a. mem[D+12] = C<<4  (D.result <-> C.prev)
    lit16 SCR+20
    load
    lit8 4
    shl
    lit16 SCR+24
    load
    lit16 12
    add
    store              ; mem[D+12] = C4
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
    store              ; mem[C+8] = D2
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
    jmp .yz
.aa_noy:
    drop
.yz:
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

; (end, add1): a=end b=add1
;   counter++; end 沿 prev 线走下去
rule_end_add1:
    dup
    load
    lit8 4
    eq
    jnz .ea_ok
    swap
.ea_ok:
    lit16 SCR
    store              ; SCR = a (end)
    lit16 SCR+4
    store              ; SCR+4 = b (add1)
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
    load               ; [z]
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
    load               ; [z]
    dup
    call pc_addr       ; [z az]
    lit16 SCR
    load
    lit8 4
    shl                ; [z az a4]
    swap               ; [z a4 az]
    store              ; mem[az] = a4 ; [z]
    drop
    ; mem[a+4] = z
    lit16 SCR+4
    load
    lit16 8
    add
    load               ; [z]
    lit16 SCR
    load
    lit16 4
    add                ; [z a+4]
    store              ; mem[a+4] = z
    lit16 SCR+4
    load
    call freeag
    ret

; (end, zero): a=end b=zero
;   counter++; 全部释放，完成
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

; ---------------- 主程序 ----------------

main:
    ; ---- 初始化全局 ----
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
    ; ---- 自由表: 链式初始化 agent 区 ----
    lit16 0x7000
    lit16 SCR+40
    store              ; SCR+40 = 区终点
    lit16 AGENTS
.init_loop:
    dup
    lit16 16
    add                ; [a n]
    dup
    lit16 SCR+40
    load               ; [a n n e]
    lt                 ; (n < e) ; [a n flag]
    jz .init_last      ; [a n]
    dup                ; [a n n]
    rot                ; [n n a]
    store              ; mem[a] = n ; [n]
    jmp .init_loop
.init_last:            ; [a n]
    drop               ; [a]
    lit16 0
    swap               ; [0 a]
    store              ; mem[a] = 0
    lit16 AGENTS
    lit16 FREE
    store              ; free_head = AGENTS
    ; ---- 规则表 ----
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
    ; ---- 构建 K+K ----
    lit16 K
    call build_chain
    lit16 SCR+44
    store              ; SCR+44 = 3-chain 头
    lit16 K
    call build_chain
    lit16 SCR+48
    store              ; SCR+48 = 2-chain 头
    lit8 3
    call newag
    lit16 SLOT+4
    store              ; SLOT+4 = add
    lit8 4
    call newag
    lit16 SLOT
    store              ; SLOT = end
    ; 3-chain头.p0 <-> add.p1 (addend)
    lit16 SCR+44
    load
    lit8 0
    call portv
    lit16 SLOT+4
    load
    lit8 1
    call portv
    call connect
    ; 2-chain头.p0 <-> add.p0 (principal)
    lit16 SCR+48
    load
    lit8 0
    call portv
    lit16 SLOT+4
    load
    lit8 0
    call portv
    call connect
    ; add.p2 (result) <-> end.p0
    lit16 SLOT+4
    load
    lit8 2
    call portv
    lit16 SLOT
    load
    lit8 0
    call portv
    call connect
    ; 初始活跃对: add
    lit16 SLOT+4
    load
    call enq
    ; ---- 驱动循环 ----
.driver:
    lit16 QHEAD
    load               ; [h]
    lit16 QTAIL
    load               ; [h t]
    dup
    rot                ; [t t h]
    eq                 ; (t == h) ; [t flag]
    jnz .main_done          ; [t]
    drop               ; []
    ; 弹出 a (环形)
    lit16 QHEAD
    load               ; [h]
    dup
    lit16 511
    and
    lit8 2
    shl
    lit16 QUEUE
    add                ; [h qa]
    load               ; [h a]
    swap               ; [a h]
    lit8 1
    add
    lit16 QHEAD
    store              ; q_head++ ; [a]
    ; 验证: typeA 有效
    dup
    load               ; [a ta]
    dup
    jnz .ta_ok
    drop
    jmp .drop
.ta_ok:
    lit8 5
    lt
    jz .drop           ; [a]
    ; B = mem[a+4]>>4
    dup
    lit16 4
    add
    load
    lit8 4
    shr                ; [a B]
    swap
    lit16 SCR
    store              ; SCR = a ; [B]
    lit16 SCR+4
    store              ; SCR+4 = B ; []
    ; back 检查: mem[B+4] == (a<<4)
    lit16 SCR
    load
    lit8 4
    shl                ; [a4]
    lit16 SCR+4
    load
    lit16 4
    add
    load               ; [a4 back]
    eq
    jz .drop           ; []
    ; typeB 有效
    lit16 SCR+4
    load
    load               ; [tb]
    dup
    jnz .tb_ok
    drop
    jmp .drop
.tb_ok:
    lit8 5
    lt
    jz .drop           ; []
    ; ---- 分发 ----
    ; SCR = a, SCR+4 = B
    ; key = (mem[a] << 4) | mem[B]
    lit16 SCR
    load
    load               ; [ta]
    lit8 4
    shl                ; [ta4]
    lit16 SCR+4
    load
    load               ; [ta4 tb]
    or                 ; [key]
    dup
    lit8 1
    shl                ; [key key2]
    lit16 TABLE
    add                ; [key taddr]
    load16             ; [key rule]
    dup
    jnz .has_rule
    drop
    jmp .norule
.has_rule:
    swap               ; [rule key]
    drop               ; [rule]
    lit16 SCR
    load               ; [rule a]
    lit16 SCR+4
    load               ; [rule a B]
    rot                ; [a B rule]
    callx              ; 规则入口 [a B]
    jmp .driver
.drop:
    jmp .driver
.norule:
    lit16 3
    lit16 ERR
    store
    halt
.main_done:
    halt
