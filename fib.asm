; fib: [n] -> [fib(n)]
; 朴素递归，模仿 ~/fibonacci/fibonacci.c 的计算方式
; 宿主每次 run 前把 n 压入参数栈（sp = sp0 - 4），entry = main
.sp0 0x1000
.entry main

fib:
    dup
    lit8 1
    le
    jz recurse
    ret
recurse:
    dup
    lit8 1
    sub
    call fib
    swap
    lit8 2
    sub
    call fib
    add
    ret

main:
    call fib
    halt
