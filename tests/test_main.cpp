#include <gtest/gtest.h>
#include <string>

class SqliteTest : public ::testing::Test 
{
protected:
    void SetUp() override {
        message = "Build and Debug Success";
    }

    void TearDown() override {
        message.clear();
    }

    std::string message;
};

TEST_F(SqliteTest, CheckMessageContent) 
{
    EXPECT_EQ(message, "Build and Debug Success");
    EXPECT_STREQ(message.c_str(), "Build and Debug Success");
}

int main(int argc, char **argv) 
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}