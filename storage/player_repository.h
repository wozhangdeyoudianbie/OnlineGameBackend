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
    explicit PlayerRepository(MYSQL *connection) noexcept;
    ~PlayerRepository() noexcept = default;
    PlayerRepository(const PlayerRepository &) = delete;
    PlayerRepository &operator=(const PlayerRepository &) = delete;
    PlayerRepository(PlayerRepository &&) = delete;
    PlayerRepository &operator=(PlayerRepository &&) = delete;
    RepositoryResult create_player(std::uint64_t player_id);
    PlayerSummaryResult get_player_summary(std::uint64_t player_id);
    MatchHistoryResult get_match_history(std::uint64_t player_id);
private:
    MYSQL *connection_;
};
