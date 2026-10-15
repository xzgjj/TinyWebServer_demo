#include <print>
#include <expected>

#if defined(TWS_HAS_PRINT)
#include <print>
#endif

int main() {
    std::expected<int, int> value{1};
    return value.has_value() ? 0 : 1;
}
