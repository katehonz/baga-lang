// fact: recursive contract + overflow (fails for large n in both tools).
#include <assert.h>
long long fact(long long n) {
    __CPROVER_assume(n >= 0);
    if (n <= 0) return 1;
    long long r = fact(n - 1);
    return n * r;
}
int main() {
    long long n;
    long long out = fact(n);
    __CPROVER_assert(out >= 1, "output >= 1");
    return 0;
}
