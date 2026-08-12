#!/bin/bash
# 跨语言 fib(40) 基准：warmvm vs C vs Python 等
# 每个实现跑 3 次取最优（消除抖动）
cd "$(dirname "$0")/.."
export TIMEFORMAT='%R'

gcc -O2 -o bench/fib40_c_O2 bench/fib40.c || exit 1
gcc -O0 -o bench/fib40_c_O0 bench/fib40.c || exit 1
JAVAC=$(find /usr/lib/jvm -name javac 2>/dev/null | sort | tail -1)
[ -n "$JAVAC" ] && "$JAVAC" -d bench bench/Fib40.java 2>/dev/null
chmod +x bench/fib40.erl

time_one() {
  local name="$1"; shift
  local best=999999999
  for i in 1 2 3; do
    local t ms
    t=$( { time "$@" >/dev/null 2>&1; } 2>&1 )
    ms=$(awk -v t="$t" 'BEGIN{printf "%d", t*1000}')
    [ "$ms" -lt "$best" ] && best=$ms
  done
  printf '%-22s %10.3f s\n' "$name" "$(awk -v b="$best" 'BEGIN{printf "%.3f", b/1000}')"
}

echo "== fib(40) 各实现耗时（3 次取最优）=="
time_one "C (-O2)"           ./bench/fib40_c_O2
time_one "C (-O0)"           ./bench/fib40_c_O0
time_one "warmvm"            ./wvm.exe --noverify 40
time_one "Python 3.14"       python3 bench/fib40.py
time_one "Lua 5.4"           lua5.4 bench/fib40.lua
time_one "LuaJIT"            luajit bench/fib40.lua
time_one "Node.js"           node bench/fib40.js
time_one "Ruby"              ruby bench/fib40.rb
[ -n "$JAVAC" ] && time_one "Java (openjdk)" java -cp bench Fib40
time_one "Guile"             guile bench/fib40.scm
time_one "Erlang (escript)"  escript bench/fib40.erl
time_one "Chez Scheme"       scheme --quiet --script bench/fib40.ss
