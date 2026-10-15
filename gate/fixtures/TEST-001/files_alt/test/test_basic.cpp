#include <gtest/gtest.h>

TEST(BasicFixture, KeepsWorking) {
    EXPECT_EQ(1 + 1, 2);
    EXPECT_TRUE(true);
}
