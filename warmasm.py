#!/usr/bin/env python3
"""warmasm —— WarmVM 汇编器（宿主侧工具，不占 VM 缓存预算）

用法: warmasm.py [-o out.wvm] in.wvm
语法:
  ; 注释
  label:
  op              ; 单词令
  op 操作数       ; lit8/lit16/lit32: 整数（0x 十六进制）；jmp/jz/jnz/call: 标签或绝对字节地址
伪指令:
  .sp0 0x1000     ; 参数栈锚点（= 代码区起点），默认 0x1000
  .entry label    ; 入口，默认第一个标签
"""
import sys, struct

OPS = {
    'halt': 0x00, 'nop': 0x01, 'lit8': 0x02, 'lit16': 0x03, 'lit32': 0x04,
    'dup': 0x05, 'drop': 0x06, 'swap': 0x07, 'over': 0x08, 'rot': 0x09,
    'add': 0x0A, 'sub': 0x0B, 'mul': 0x0C, 'div': 0x0D, 'mod': 0x0E,
    'and': 0x0F, 'or': 0x10, 'xor': 0x11, 'shl': 0x12, 'shr': 0x13,
    'eq': 0x14, 'ne': 0x15, 'lt': 0x16, 'gt': 0x17, 'le': 0x18, 'ge': 0x19,
    'load': 0x1A, 'store': 0x1B, 'load8': 0x1C, 'store8': 0x1D,
    'load16': 0x1E, 'store16': 0x1F,
    'jmp': 0x20, 'jz': 0x21, 'jnz': 0x22, 'call': 0x23,
    'ret': 0x24, 'jmpx': 0x25, 'callx': 0x26,
    # 融合指令（peephole 生成）
    'dec': 0x27, 'duple': 0x28, 'subk': 0x29,
}
LIT_N = {'lit8': 1, 'lit16': 2, 'lit32': 4,
         'duple': 1, 'subk': 1}   # 立即数字节数
BR_N = {'jmp': 2, 'jz': 2, 'jnz': 2, 'call': 2}  # 目标字节数

def parse(path):
    lines = []
    for raw in open(path).read().splitlines():
        s = raw.split(';')[0].strip()
        if not s:
            continue
        if s.endswith(':'):
            lines.append(('label', s[:-1].strip(), []))
        else:
            p = s.split()
            lines.append(('insn', p[0].lower(), p[1:]))
    return lines

def insn_size(op):
    if op in LIT_N:
        return 1 + LIT_N[op]
    if op in BR_N:
        return 1 + BR_N[op]
    return 1

def parse_int(tok):
    return int(tok, 0)

def resolve_operand(tok, eqs, labels, code_base):
    """操作数解析：.eq 常量 > 标签（代码地址）> 整数；支持 + - * 表达式"""
    tok = tok.strip()
    if tok in eqs:
        return eqs[tok]
    if tok in labels:
        return code_base + labels[tok]
    # 表达式：先把 .eq 名替换为值，再安全求值
    expr = tok
    for k, v in sorted(eqs.items(), key=lambda kv: -len(kv[0])):
        expr = expr.replace(k, str(v))
    if all(c in '0123456789abcdefxABCDEFX+-* ()' for c in expr):
        return int(eval(expr, {'__builtins__': {}}))
    return int(tok, 0)


def peephole(lines):
    """基本块内的指令融合（不跨标签）：
      dup lit8 k le  -> duple k
      lit8 1 sub     -> dec
      lit8 k sub     -> subk k
    """
    out = []
    i, n = 0, len(lines)
    while i < n:
        kind, name, args = lines[i]
        if kind == 'label':
            out.append(lines[i]); i += 1; continue
        if name == 'dup' and i + 2 < n:
            n2, n3 = lines[i + 1], lines[i + 2]
            if n2[0] == 'insn' and n2[1] == 'lit8' and n3[0] == 'insn' and n3[1] == 'le':
                k = parse_int(n2[2][0])
                if -128 <= k <= 127:
                    out.append(('insn', 'duple', [str(k)])); i += 3; continue
        if name == 'lit8' and i + 1 < n:
            n2 = lines[i + 1]
            if n2[0] == 'insn' and n2[1] == 'sub':
                k = parse_int(args[0])
                if -128 <= k <= 127:
                    if k == 1:
                        out.append(('insn', 'dec', []))
                    else:
                        out.append(('insn', 'subk', [str(k)]))
                    i += 2; continue
        out.append(lines[i]); i += 1
    return out

def main():
    argv = sys.argv[1:]
    out = None
    show_symbols = '--symbols' in argv
    argv = [a for a in argv if a != '--symbols']
    if '-o' in argv:
        i = argv.index('-o'); out = argv[i + 1]; del argv[i:i + 2]
    if not argv:
        print(__doc__); sys.exit(1)
    src = argv[0]

    lines = peephole(parse(src))
    sp0 = 0x1000
    entry_name = None
    # 第一遍：收集 .eq；标签 → 代码区偏移；算代码长度
    labels, off = {}, 0
    eqs = {}
    for kind, name, args in lines:
        if kind == 'label':
            if name in labels:
                sys.exit(f'重复标签: {name}')
            labels[name] = off
        else:
            if name == '.eq':
                eqs[args[0]] = parse_int(args[1])
            elif name == '.sp0':
                sp0 = parse_int(args[0])
            elif name == '.entry':
                entry_name = args[0]
            elif name in OPS:
                off += insn_size(name)
            else:
                sys.exit(f'未知指令: {name}')
    if entry_name is None:
        entry_name = next(iter(labels))
    code_base = sp0
    code_size = off
    entry = code_base + labels[entry_name]

    if show_symbols:
        for name, o in sorted(labels.items(), key=lambda kv: kv[1]):
            print(f"{name} 0x{code_base + o:04x}")

    # 第二遍：发射
    code = bytearray()
    for kind, name, args in lines:
        if kind == 'label':
            continue
        if name in ('.sp0', '.entry', '.eq'):
            continue
        op = OPS[name]
        code.append(op)
        if name in LIT_N:
            v = resolve_operand(args[0], eqs, labels, code_base)
            code += int(v).to_bytes(LIT_N[name], 'little', signed=True)
        elif name in BR_N:
            t = resolve_operand(args[0], eqs, labels, code_base)
            code += t.to_bytes(2, 'little')

    hdr = struct.pack('<HHHHHHHH',
                      WVM_MAGIC := 0x4D57, entry, 0x0080, sp0,
                      len(code), 0, 0, 0)
    blob = hdr + bytes(code)
    if out:
        open(out, 'wb').write(blob)
        print(f'{src} -> {out}: code {len(code)}B @0x{sp0:x}, entry 0x{entry:x}')
    else:
        sys.stdout.buffer.write(blob)

if __name__ == '__main__':
    main()
