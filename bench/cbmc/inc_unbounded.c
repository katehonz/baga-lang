// inc_unbounded: n >= 0 leaves n+1 open to n = INT64_MAX (overflow refuted).
#include <assert.h>
int inc(int n) {
    __CPROVER_assume(n >= 0);
    return n + 1;
}
int main() {
    int n;
    int out = inc(n);
    __CPROVER_assert(out >= 1, "output >= 1");
    return 0;
}
