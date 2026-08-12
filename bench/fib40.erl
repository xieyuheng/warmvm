#!/usr/bin/env escript
%%! -noshell
fib(0) -> 0;
fib(1) -> 1;
fib(N) -> fib(N-1) + fib(N-2).
main(_) -> io:format("~p~n", [fib(40)]).
