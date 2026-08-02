// inc_bounded: 0 <= n <= 100 ⇒ n+1 cannot overflow; ensures output >= 1.
#include <assert.h>
int inc(int n) {
    __CPROVER_assume(0 <= n && n <= 100);
    return n + 1;
}
int main() {
    int n;
    int out = inc(n);
    __CPROVER_assert(out >= 1, "output >= 1");
    return 0;
}
