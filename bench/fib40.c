#include <stdio.h>
/* gcc -O2 -o fib40_c fib40.c */
int fib(int n) { return n <= 1 ? n : fib(n - 1) + fib(n - 2); }
int main(void) { printf("%d\n", fib(40)); return 0; }
