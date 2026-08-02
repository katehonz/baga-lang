// div_zero: n / m with m >= 1 is safe; without it, m = 0 refutes.
#include <assert.h>
long long dvs(long long n, long long m) {
    __CPROVER_assume(n >= 0 && m >= 1);
    return n / m;
}
int main() {
    long long n, m;
    long long out = dvs(n, m);
    __CPROVER_assert(out >= 0, "output >= 0");
    return 0;
}
