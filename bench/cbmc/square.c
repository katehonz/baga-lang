// square: n*n >= 0 (nonlinear, sign axioms vs bit-blasting).
#include <assert.h>
long long sq(long long n) {
    __CPROVER_assume(-1000000LL <= n && n <= 1000000LL);
    return n * n;
}
int main() {
    long long n;
    long long out = sq(n);
    __CPROVER_assert(out >= 0, "output >= 0");
    return 0;
}
