#include "storage/redis_client.h"
#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>

namespace
{
    std::string redis_host()
    {
        const char *value = std::getenv("P2_REDIS_HOST");
        return value != nullptr ? value : "127.0.0.1";
    }

    int redis_port()
    {
        const char *value = std::getenv("P2_REDIS_PORT");
        return value != nullptr ? std::stoi(value) : 6379;
    }

    class RedisClientTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            client_ = std::make_unique<RedisClient>(redis_host(), redis_port(), timeval{1, 0});
            ASSERT_TRUE(client_->ready()) << "Redis 未就绪：" << client_->error_message();
            key_ = "p2:test:redis_client:" + std::to_string(static_cast<long long>(::getpid()));
            RedisCommandResult result = client_->command({"DEL", key_});
            ASSERT_EQ(result.status, RedisCommandStatus::Ok) << result.message;
        }

        void TearDown() override
        {
            if (client_ && client_->ready())
            {
                client_->command({"DEL", key_});
            }
        }

        std::unique_ptr<RedisClient> client_;
        std::string key_;
    };
}

TEST_F(RedisClientTest, BinarySafeSetGetTtlAndDel)
{
    const std::string value("A\0B C", 5);
    RedisCommandResult set_result = client_->command({"SET", key_, value, "EX", "60"});
    ASSERT_EQ(set_result.status, RedisCommandStatus::Ok) << set_result.message;
    ASSERT_NE(set_result.reply, nullptr);
    ASSERT_EQ(set_result.reply->type, REDIS_REPLY_STATUS);
    EXPECT_EQ(std::string(set_result.reply->str, set_result.reply->len), "OK");

    RedisCommandResult get_result = client_->command({"GET", key_});
    ASSERT_EQ(get_result.status, RedisCommandStatus::Ok) << get_result.message;
    ASSERT_NE(get_result.reply, nullptr);
    ASSERT_EQ(get_result.reply->type, REDIS_REPLY_STRING);
    EXPECT_EQ(std::string(get_result.reply->str, get_result.reply->len), value);

    RedisCommandResult ttl_result = client_->command({"TTL", key_});
    ASSERT_EQ(ttl_result.status, RedisCommandStatus::Ok) << ttl_result.message;
    ASSERT_NE(ttl_result.reply, nullptr);
    ASSERT_EQ(ttl_result.reply->type, REDIS_REPLY_INTEGER);
    EXPECT_GE(ttl_result.reply->integer, 0);
    EXPECT_LE(ttl_result.reply->integer, 60);

    RedisCommandResult del_result = client_->command({"DEL", key_});
    ASSERT_EQ(del_result.status, RedisCommandStatus::Ok) << del_result.message;
    ASSERT_NE(del_result.reply, nullptr);
    ASSERT_EQ(del_result.reply->type, REDIS_REPLY_INTEGER);
    EXPECT_EQ(del_result.reply->integer, 1);

    RedisCommandResult missing_result = client_->command({"GET", key_});
    ASSERT_EQ(missing_result.status, RedisCommandStatus::Ok) << missing_result.message;
    ASSERT_NE(missing_result.reply, nullptr);
    EXPECT_EQ(missing_result.reply->type, REDIS_REPLY_NIL);
    EXPECT_TRUE(client_->ready());
}

TEST_F(RedisClientTest, InvalidArgumentAndRedisErrorRemainDistinct)
{
    RedisCommandResult invalid_result = client_->command({});
    EXPECT_EQ(invalid_result.status, RedisCommandStatus::InvalidArgument);
    EXPECT_EQ(invalid_result.reply, nullptr);
    EXPECT_TRUE(client_->ready());

    RedisCommandResult error_result = client_->command({"GET"});
    ASSERT_EQ(error_result.status, RedisCommandStatus::RedisError);
    ASSERT_NE(error_result.reply, nullptr);
    EXPECT_EQ(error_result.reply->type, REDIS_REPLY_ERROR);
    EXPECT_TRUE(client_->ready());

    RedisCommandResult ping_result = client_->command({"PING"});
    ASSERT_EQ(ping_result.status, RedisCommandStatus::Ok) << ping_result.message;
    ASSERT_NE(ping_result.reply, nullptr);
    ASSERT_EQ(ping_result.reply->type, REDIS_REPLY_STATUS);
    EXPECT_EQ(std::string(ping_result.reply->str, ping_result.reply->len), "PONG");
}

TEST_F(RedisClientTest, CommandTimeoutMakesConnectionUnavailable)
{
    RedisClient short_client(redis_host(), redis_port(), timeval{0, 100000});
    ASSERT_TRUE(short_client.ready()) << "短超时连接创建失败：" << short_client.error_message();

    RedisCommandResult pause_result = client_->command({"CLIENT", "PAUSE", "1500"});
    ASSERT_EQ(pause_result.status, RedisCommandStatus::Ok) << "无法暂停测试 Redis：" << pause_result.message;

    RedisCommandResult timeout_result = short_client.command({"PING"});
    std::this_thread::sleep_for(std::chrono::milliseconds(1600));

    EXPECT_EQ(timeout_result.status, RedisCommandStatus::Unavailable);
    EXPECT_EQ(timeout_result.reply, nullptr);
    EXPECT_FALSE(short_client.ready());
    EXPECT_FALSE(short_client.error_message().empty());

    RedisCommandResult ping_result = client_->command({"PING"});
    ASSERT_EQ(ping_result.status, RedisCommandStatus::Ok) << ping_result.message;
}

TEST_F(RedisClientTest, RepeatedRepliesAreReleasedByRaii)
{
    for (int i = 0; i < 1000; ++i)
    {
        RedisCommandResult result = client_->command({"PING"});
        ASSERT_EQ(result.status, RedisCommandStatus::Ok) << "第 " << i << " 次 PING 失败：" << result.message;
        ASSERT_NE(result.reply, nullptr);
    }
}
