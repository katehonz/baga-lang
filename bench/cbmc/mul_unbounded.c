// mul_unbounded: a,b >= 0 leaves a*b open to overflow (refuted).
#include <assert.h>
long long mul(long long a, long long b) {
    __CPROVER_assume(a >= 0 && b >= 0);
    return a * b;
}
int main() {
    long long a, b;
    mul(a, b);   // --signed-overflow-check refutes
    return 0;
}
