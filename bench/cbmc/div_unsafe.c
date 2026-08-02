// div_unsafe: n / m without bounds — m = 0 is a counterexample.
#include <assert.h>
long long dvs(long long n, long long m) {
    return n / m;
}
int main() {
    long long n, m;
    long long out = dvs(n, m);
    __CPROVER_assert(out >= 0, "output >= 0");
    return 0;
}
