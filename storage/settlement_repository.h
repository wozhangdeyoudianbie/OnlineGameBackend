#pragma once

#include <mysql.h>
#include <cstdint>
#include <optional>
#include <string>

enum class SettlementStates
{
    Success,
    Replayed,
    InvalidRequest,
    RequestConflict,
    MatchNotFound,
    MatchAlreadySettled,
    ParticipantMismatch,
    RetryableError,
    CommitOutcomeUnknown,
    ConnectionError,
    SqlError,
    DataError
};

struct SettlementError
{
    unsigned int code{0};
    std::string message;
};

struct ReportMatchResultRequest
{
    std::string request_id;
    std::string match_id;
    std::uint64_t winner_player_id{0};
    std::uint64_t loser_player_id{0};
};

struct MatchSettlement
{
    std::string request_id;
    std::string match_id;
    std::uint64_t winner_player_id{0};
    std::uint64_t loser_player_id{0};
};

struct ReportMatchResultResult
{
    SettlementStates state{SettlementStates::DataError};
    std::optional<MatchSettlement> settlement;
    SettlementError error;
};

class SettlementRepository final
{
public:
    // 只保存连接指针，不接管其所有权。
    explicit SettlementRepository(MYSQL *connection) noexcept;

    // 连接所有权不归本类，析构无需释放资源。
    ~SettlementRepository() noexcept = default;

    // 只持有连接指针，禁止拷贝与移动。
    SettlementRepository(const SettlementRepository &) = delete;
    SettlementRepository &operator=(const SettlementRepository &) = delete;
    SettlementRepository(SettlementRepository &&) = delete;
    SettlementRepository &operator=(SettlementRepository &&) = delete;

    // 结算入口：在事务内锁定对局并落库全部结算效果，任一步失败回滚。
    ReportMatchResultResult report_match_result(const ReportMatchResultRequest &request);
private:
    MYSQL *connection_;
};
