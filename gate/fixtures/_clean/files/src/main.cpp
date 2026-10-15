#include <cstddef>

namespace {

std::size_t add(std::size_t left, std::size_t right) {
    const std::size_t sum = left + right;
    return sum;
}

std::size_t clamp_to(std::size_t value, std::size_t upper) {
    if (value > upper) {
        return upper;
    }
    return value;
}

}  // namespace

int main() {
    const std::size_t total = add(1, 2);
    const std::size_t bounded = clamp_to(total, 100);
    return bounded == 3 ? 0 : 1;
}
