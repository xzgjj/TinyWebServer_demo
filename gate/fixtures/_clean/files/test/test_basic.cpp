#include <cstddef>

namespace {

std::size_t doubled(std::size_t value) {
    return value * 2;
}

}  // namespace

int main() {
    if (doubled(2) != 4) {
        return 1;
    }
    return 0;
}
