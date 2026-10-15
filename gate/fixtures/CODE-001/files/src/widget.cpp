#include <cstddef>

namespace fixture {

class Widget {
public:
    Widget() = default;
    void run();

private:
    int used_;
    int never_used_;
};

void Widget::run() {
    used_ = 1;
}

// int old_helper(int value) {
//     int total = 0;
//     for (int i = 0; i < value; ++i) {
//         total += i;
//     }
//     return total;
// }
//
// void old_reset() {
//     used_ = 0;
// }
//
// void old_flush() {
//     used_ = 0;
// }
//
// void old_close() {
//     used_ = 0;
// }

static int orphan_helper(int value) {
    return value * 2;
}

}  // namespace fixture
