// mul_bounded: |a|,|b| <= 1e9 ⇒ a*b fits i64.
#include <assert.h>
long long mul(long long a, long long b) {
    __CPROVER_assume(-1000000000LL <= a && a <= 1000000000LL);
    __CPROVER_assume(-1000000000LL <= b && b <= 1000000000LL);
    return a * b;
}
int main() {
    long long a, b;
    mul(a, b);   // --signed-overflow-check proves no overflow
    return 0;
}
