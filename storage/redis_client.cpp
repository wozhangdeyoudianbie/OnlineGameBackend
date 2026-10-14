#include "redis_client.h"
#include <limits>
#include <utility>

RedisClient::RedisClient(const std::string &host, int port, timeval timeout)
    : context_(redisConnectWithTimeout(host.c_str(), port, timeout))
{
    if (!context_)
    {
        error_message_ = "Redis 连接失败：未创建连接上下文";
        return;
    }
    if (context_->err != 0)
    {
        error_message_ = "Redis 连接失败：" + std::string(context_->errstr);
        context_.reset();
        return;
    }
    if (redisSetTimeout(context_.get(), timeout) != REDIS_OK)
    {
        error_message_ = "设置 Redis 命令超时失败";
        context_.reset();
    }
}

bool RedisClient::ready() const
{
    return static_cast<bool>(context_);
}

const std::string &RedisClient::error_message() const
{
    return error_message_;
}

RedisCommandResult RedisClient::command(const std::vector<std::string> &arguments)
{
    if (arguments.empty())
    {
        return {RedisCommandStatus::InvalidArgument, RedisReplyPtr(nullptr), "Redis 命令参数不能为空"};
    }
    if (arguments.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
    {
        return {RedisCommandStatus::InvalidArgument, RedisReplyPtr(nullptr), "Redis 命令参数数量超出范围"};
    }
    if (!context_)
    {
        return {RedisCommandStatus::Unavailable, RedisReplyPtr(nullptr), "Redis 连接当前不可用"};
    }
    std::vector<const char *> argument_data;
    std::vector<size_t> argument_lengths;
    argument_data.reserve(arguments.size());
    argument_lengths.reserve(arguments.size());
    for (const std::string &argument : arguments)
    {
        argument_data.push_back(argument.data());
        argument_lengths.push_back(argument.size());
    }
    const int argument_count = static_cast<int>(arguments.size());
    RedisReplyPtr reply(static_cast<redisReply *>(redisCommandArgv(context_.get(), argument_count, argument_data.data(), argument_lengths.data())));
    if (!reply)
    {
        std::string message = "Redis 命令执行失败";
        if (context_->errstr[0] != '\0')
        {
            message += "：";
            message += context_->errstr;
        }
        error_message_ = message;
        context_.reset();
        return {RedisCommandStatus::Unavailable, RedisReplyPtr(nullptr), std::move(message)};
    }
    if (reply->type == REDIS_REPLY_ERROR)
    {
        std::string message = "Redis 返回命令错误";
        if (reply->str != nullptr)
        {
            message += "：";
            message.append(reply->str, reply->len);
        }
        return {RedisCommandStatus::RedisError, std::move(reply), std::move(message)};
    }
    return {RedisCommandStatus::Ok, std::move(reply), ""};
}
