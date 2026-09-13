#include <gtest/gtest.h>

TEST(ProjectSmokeTest, UsesCpp17OrLater)
{
    EXPECT_GE(__cplusplus, 201703L);
}
