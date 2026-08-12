public class Fib40 {
    static int fib(int n) { return n <= 1 ? n : fib(n - 1) + fib(n - 2); }
    public static void main(String[] a) { System.out.println(fib(40)); }
}
