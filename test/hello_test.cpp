#include <gtest/gtest.h>
#include <gmock/gmock.h>

TEST(HelloTest, BasicAssertion)
{
    EXPECT_EQ(1 + 1, 2);
}

TEST(HelloTest, StringTest)
{
    std::string hello = "Hello World!";
    EXPECT_THAT(hello, testing::HasSubstr("World"));
}

int main(int argc, char** argv)
{
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
