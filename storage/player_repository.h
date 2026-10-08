#pragma once

#include <mysql.h>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

enum class RepositoryStates
{
    Success,
    NotFound,
    ConnectionError,
    SqlError,
    DataError
};

struct RepositoryError
{
    unsigned int code{0};
    std::string message;
};

struct RepositoryResult
{
    RepositoryStates state{RepositoryStates::DataError};
    RepositoryError error;
};

struct PlayerSummary
{
    std::uint64_t player_id{0};
    std::string created_at;
    std::optional<std::string> active_match_id;
};

struct PlayerSummaryResult
{
    RepositoryStates state{RepositoryStates::DataError};
    std::optional<PlayerSummary> summary;
    RepositoryError error;
};

struct MatchHistoryItem
{
    std::string match_id;
    unsigned int state{0};
    std::string created_at;
    std::optional<std::string> completed_at;
};

struct MatchHistoryResult
{
    RepositoryStates state{RepositoryStates::DataError};
    std::vector<MatchHistoryItem> matches;
    RepositoryError error;
};

class PlayerRepository final
{
public:
    // 只保存连接指针，不接管其所有权。
    explicit PlayerRepository(MYSQL *connection) noexcept;

    // 连接所有权不归本类，析构无需释放资源。
    ~PlayerRepository() noexcept = default;

    // 只持有连接指针，禁止拷贝与移动。
    PlayerRepository(const PlayerRepository &) = delete;
    PlayerRepository &operator=(const PlayerRepository &) = delete;
    PlayerRepository(PlayerRepository &&) = delete;
    PlayerRepository &operator=(PlayerRepository &&) = delete;

    // 插入一名玩家；连接为空返回 ConnectionError，语句失败按错误码归类。
    RepositoryResult create_player(std::uint64_t player_id);

    // 查询玩家概要：创建时间与当前在局分配的比赛 ID；玩家不存在返回 NotFound。
    PlayerSummaryResult get_player_summary(std::uint64_t player_id);

    // 查询玩家最近 20 场对局，按创建时间倒序；没有对局时返回空列表。
    MatchHistoryResult get_match_history(std::uint64_t player_id);
private:
    MYSQL *connection_;
};
