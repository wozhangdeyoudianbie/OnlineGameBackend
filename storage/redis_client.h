#pragma once

#include <hiredis/hiredis.h>
#include <sys/time.h>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

struct RedisContextDeleter
{
    void operator()(redisContext *context) const
    {
        if (context != nullptr)
        {
            redisFree(context);
        }
    }
};

struct RedisReplyDeleter
{
    void operator()(redisReply *reply) const
    {
        if (reply != nullptr)
        {
            freeReplyObject(reply);
        }
    }
};

using RedisContextPtr = std::unique_ptr<redisContext, RedisContextDeleter>;
using RedisReplyPtr = std::unique_ptr<redisReply, RedisReplyDeleter>;

enum class RedisCommandStatus
{
    Ok,
    InvalidArgument,
    RedisError,
    Unavailable
};

struct RedisCommandResult
{
    RedisCommandStatus status;
    RedisReplyPtr reply;
    std::string message;
};

class RedisClient
{
public:
    RedisClient(const std::string &host, int port, timeval timeout);
    bool ready() const;
    const std::string &error_message() const;
    RedisCommandResult command(const std::vector<std::string> &arguments);
private:
    RedisContextPtr context_;
    std::string error_message_;
};
