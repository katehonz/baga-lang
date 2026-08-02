// abs_val: output >= 0 provable, but abs(INT64_MIN) overflows
// (CBMC --signed-overflow-check should catch it too).
#include <assert.h>
long long abs_val(long long x) {
    if (x < 0) return 0 - x;
    return x;
}
int main() {
    long long x;
    long long out = abs_val(x);
    __CPROVER_assert(out >= 0, "output >= 0");
    return 0;
}
