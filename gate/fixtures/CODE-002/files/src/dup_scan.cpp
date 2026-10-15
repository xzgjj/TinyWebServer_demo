#include <cstddef>

namespace fixture {

int alpha_scan(int value) {
    int total = 0;
    for (int i = 0; i < value; ++i) {
        total += i;
        if (total > 100) {
            total -= 1;
        }
        if (i % 2 == 0) {
            total += 2;
        }
    }
    return total;
}

int beta_scan(int value) {
    int total = 0;
    for (int i = 0; i < value; ++i) {
        total += i;
        if (total > 100) {
            total -= 1;
        }
        if (i % 2 == 0) {
            total += 2;
        }
    }
    return total;
}

}  // namespace fixture
