// sum_loop: invariant-based loop proof (M1 + havoc discipline).
#include <assert.h>
long long add_repeated(long long k, long long m) {
    __CPROVER_assume(k >= 0 && m >= 0);
    long long i = 0, s = 0;
    while (i < m) {
        __CPROVER_assert(s >= 0 && i >= 0, "invariant");
        s = s + k;
        i = i + 1;
    }
    return s;
}
int main() {
    long long k, m;
    long long out = add_repeated(k, m);
    __CPROVER_assert(out >= 0, "output >= 0");
    return 0;
}
