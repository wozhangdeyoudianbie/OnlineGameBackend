#include "settlement_repository.h"
#include "mysql_RAII.h"
#include <mysql.h>
#include <cstring>
#include <string>
#include <array>
#include <algorithm>

namespace
{
    constexpr const char *kStartTransactionSql = "START TRANSACTION";
    constexpr const char *kLockMatchSql =
        "SELECT state "
        "FROM matches "
        "WHERE match_id = ? "
        "FOR UPDATE";
    constexpr const char *kInsertResultRequestSql =
        "INSERT INTO result_requests "
        "(request_id, match_id, winner_player_id, loser_player_id) "
        "VALUES (?, ?, ?, ?)";
    constexpr const char *kIncrementWinnerSql =
        "UPDATE players "
        "SET win_count = win_count + 1 "
        "WHERE player_id = ?";
    constexpr const char *kIncrementLoserSql =
        "UPDATE players "
        "SET loss_count = loss_count + 1 "
        "WHERE player_id = ?";
    constexpr const char *kSetOutcomeSql =
        "UPDATE match_players "
        "SET outcome = ? "
        "WHERE match_id = ? "
        "AND player_id = ? "
        "AND outcome IS NULL";
    constexpr const char *kClearActiveAssignmentsSql =
        "DELETE FROM active_assignments "
        "WHERE match_id = ? "
        "AND player_id IN (?, ?)";
    constexpr const char *kCompleteMatchSql =
        "UPDATE matches "
        "SET state = 1, completed_at = CURRENT_TIMESTAMP(6) "
        "WHERE match_id = ? "
        "AND state = 0";
    constexpr const char *kFindRequestForUpdateSql =
        "SELECT match_id, winner_player_id, loser_player_id "
        "FROM result_requests "
        "WHERE request_id = ? "
        "FOR UPDATE";
    constexpr const char *kLockParticipantsSql =
        "SELECT player_id "
        "FROM match_players "
        "WHERE match_id = ? "
        "ORDER BY player_id "
        "FOR UPDATE";
    constexpr const char *kLockPlayerSql =
        "SELECT player_id "
        "FROM players "
        "WHERE player_id = ? "
        "FOR UPDATE";
    constexpr unsigned int kLockWaitTimeoutError = 1205;
    constexpr unsigned int kDeadlockError = 1213;

    // 判断错误码是否为连接断开（服务端消失、连接丢失）。
    bool is_disconnect_error(unsigned int error_code) noexcept
    {
        return error_code == CR_SERVER_GONE_ERROR || error_code == CR_SERVER_LOST;
    }

    // 判断错误码是否为可重试的并发冲突（锁等待超时、死锁）。
    bool is_retryable_error(unsigned int error_code) noexcept
    {
        return error_code == kLockWaitTimeoutError || error_code == kDeadlockError;
    }

    // 把 MySQL 错误码归类为结算状态：断线、可重试，其余为 SqlError。
    SettlementStates classify_sql_error(unsigned int error_code) noexcept
    {
        if (is_disconnect_error(error_code))
        {
            return SettlementStates::ConnectionError;
        }
        if (is_retryable_error(error_code))
        {
            return SettlementStates::RetryableError;
        }
        return SettlementStates::SqlError;
    }

    struct StepResult
    {
        SettlementStates state{SettlementStates::Success};
        SettlementError error;

        // 本步骤是否成功。
        bool ok() const noexcept
        {
            return state == SettlementStates::Success;
        }
    };

    // 从连接取出错误码与错误信息，并归类为结算状态。
    StepResult connection_failure(MYSQL *connection)
    {
        const unsigned int error_code = mysql_errno(connection);
        return {classify_sql_error(error_code), {error_code, mysql_error(connection)}};
    }

    // 从语句取出错误码与错误信息，并归类为结算状态。
    StepResult statement_failure(MYSQL_STMT *statement)
    {
        const unsigned int error_code = mysql_stmt_errno(statement);
        return {classify_sql_error(error_code), {error_code, mysql_stmt_error(statement)}};
    }

    // 绑定字符串参数：类型为 STRING，长度取当前大小。
    void bind_text(MYSQL_BIND &bind, std::string &value, unsigned long &length)
    {
        length = static_cast<unsigned long>(value.size());
        bind.buffer_type = MYSQL_TYPE_STRING;
        bind.buffer = value.data();
        bind.buffer_length = length;
        bind.length = &length;
    }

    // 绑定无符号 64 位整数参数。
    void bind_unsigned_long_long(MYSQL_BIND &bind, unsigned long long &value)
    {
        bind.buffer_type = MYSQL_TYPE_LONGLONG;
        bind.buffer = &value;
        bind.is_unsigned = true;
    }

    // 绑定无符号 TINYINT 参数。
    void bind_unsigned_tiny(MYSQL_BIND &bind, unsigned char &value)
    {
        bind.buffer_type = MYSQL_TYPE_TINY;
        bind.buffer = &value;
        bind.is_unsigned = true;
    }

    // 预处理、绑定并执行一条写语句，并校验影响行数是否等于预期。
    StepResult execute_command(MYSQL *connection, const char *sql, MYSQL_BIND *parameters, my_ulonglong expected_affected_rows, SettlementStates unexpected_affected_rows_state)
    {
        MYSQL_STMT *new_statement = mysql_stmt_init(connection);
        if (new_statement == nullptr)
        {
            return connection_failure(connection);
        }
        MysqlStatement statement(new_statement);
        if (mysql_stmt_prepare(statement.get(), sql, static_cast<unsigned long>(std::strlen(sql))) != 0)
        {
            return statement_failure(statement.get());
        }
        if (mysql_stmt_bind_param(statement.get(), parameters) != 0)
        {
            return statement_failure(statement.get());
        }
        if (mysql_stmt_execute(statement.get()) != 0)
        {
            return statement_failure(statement.get());
        }
        const my_ulonglong affected_rows = mysql_stmt_affected_rows(statement.get());
        if (affected_rows != expected_affected_rows)
        {
            return {unexpected_affected_rows_state, {0, "unexpected affected rows: expected " + std::to_string(expected_affected_rows) + ", got " + std::to_string(affected_rows)}};
        }
        return {};
    }

    struct RequestLookupResult
    {
        StepResult step;
        std::optional<MatchSettlement> settlement;
    };

    // 查询并锁定 request_id；不存在时返回 Success + nullopt。
    RequestLookupResult find_request_for_update(MYSQL *connection, const std::string &request_id)
    {
        MYSQL_STMT *new_statement = mysql_stmt_init(connection);
        if (new_statement == nullptr)
        {
            return {connection_failure(connection), std::nullopt};
        }
        MysqlStatement statement(new_statement);
        if (mysql_stmt_prepare(statement.get(), kFindRequestForUpdateSql, static_cast<unsigned long>(std::strlen(kFindRequestForUpdateSql))) != 0)
        {
            return {statement_failure(statement.get()), std::nullopt};
        }
        std::string bound_request_id = request_id;
        unsigned long request_id_length{0};
        MYSQL_BIND parameter_bind[1]{};
        bind_text(parameter_bind[0], bound_request_id, request_id_length);
        if (mysql_stmt_bind_param(statement.get(), parameter_bind) != 0)
        {
            return {statement_failure(statement.get()), std::nullopt};
        }
        if (mysql_stmt_execute(statement.get()) != 0)
        {
            return {statement_failure(statement.get()), std::nullopt};
        }
        MysqlStatementResult result(statement.get());
        std::array<char, 64> match_id_buffer{};
        unsigned long match_id_length = 0;
        unsigned long long winner_player_id = 0;
        unsigned long long loser_player_id = 0;
        bool match_id_is_null = false;
        bool winner_player_id_null = false;
        bool loser_player_id_null = false;
        bool winner_player_id_error = false;
        bool loser_player_id_error = false;
        bool match_id_error = false;
        MYSQL_BIND result_bind[3]{};
        result_bind[0].buffer_type = MYSQL_TYPE_STRING;
        result_bind[0].buffer = match_id_buffer.data();
        result_bind[0].buffer_length = static_cast<unsigned long>(match_id_buffer.size());
        result_bind[0].length = &match_id_length;
        result_bind[0].is_null = &match_id_is_null;
        result_bind[0].error = &match_id_error;
        result_bind[1].buffer_type = MYSQL_TYPE_LONGLONG;
        result_bind[1].buffer = &winner_player_id;
        result_bind[1].is_unsigned = true;
        result_bind[1].is_null = &winner_player_id_null;
        result_bind[1].error = &winner_player_id_error;
        result_bind[2].buffer_type = MYSQL_TYPE_LONGLONG;
        result_bind[2].buffer = &loser_player_id;
        result_bind[2].is_unsigned = true;
        result_bind[2].is_null = &loser_player_id_null;
        result_bind[2].error = &loser_player_id_error;
        if (mysql_stmt_bind_result(statement.get(), result_bind) != 0)
        {
            return {statement_failure(statement.get()), std::nullopt};
        }
        const int fetch_state = mysql_stmt_fetch(statement.get());
        if (fetch_state == MYSQL_NO_DATA)
        {
            return {{}, std::nullopt};
        }
        if (fetch_state == MYSQL_DATA_TRUNCATED)
        {
            return {{SettlementStates::DataError, {0, "result_requests row was truncated"}}, std::nullopt};
        }
        if (fetch_state != 0)
        {
            return {statement_failure(statement.get()), std::nullopt};
        }
        if (match_id_is_null || winner_player_id_null || loser_player_id_null || match_id_error || winner_player_id_error || loser_player_id_error || match_id_length == 0 || match_id_length > match_id_buffer.size() || winner_player_id == 0 || loser_player_id == 0 || winner_player_id == loser_player_id)
        {
            return {{SettlementStates::DataError, {0, "invalid result_requests row"}}, std::nullopt};
        }
        MatchSettlement settlement{request_id, std::string(match_id_buffer.data(), static_cast<std::size_t>(match_id_length)), static_cast<std::uint64_t>(winner_player_id), static_cast<std::uint64_t>(loser_player_id)};
        return {{}, settlement};
    }

    // 锁定仍处于开放状态的对局，并在内部完成状态映射。
    StepResult lock_open_match(MYSQL *connection, const std::string &match_id)
    {
        MYSQL_STMT *new_statement = mysql_stmt_init(connection);
        if (new_statement == nullptr)
        {
            return connection_failure(connection);
        }
        MysqlStatement statement(new_statement);
        if (mysql_stmt_prepare(statement.get(), kLockMatchSql, static_cast<unsigned long>(std::strlen(kLockMatchSql))) != 0)
        {
            return statement_failure(statement.get());
        }
        std::string bound_match_id = match_id;
        unsigned long match_id_length{0};
        MYSQL_BIND parameter_bind[1]{};
        bind_text(parameter_bind[0], bound_match_id, match_id_length);
        if (mysql_stmt_bind_param(statement.get(), parameter_bind) != 0)
        {
            return statement_failure(statement.get());
        }
        if (mysql_stmt_execute(statement.get()) != 0)
        {
            return statement_failure(statement.get());
        }
        MysqlStatementResult result(statement.get());
        unsigned char state = 0;
        bool state_is_null = false;
        bool state_error = false;
        MYSQL_BIND result_bind[1]{};
        result_bind[0].buffer_type = MYSQL_TYPE_TINY;
        result_bind[0].is_unsigned = true;
        result_bind[0].buffer = &state;
        result_bind[0].is_null = &state_is_null;
        result_bind[0].error = &state_error;
        if (mysql_stmt_bind_result(statement.get(), result_bind) != 0)
        {
            return statement_failure(statement.get());
        }
        const int fetch_state = mysql_stmt_fetch(statement.get());
        if (fetch_state == MYSQL_NO_DATA)
        {
            return {SettlementStates::MatchNotFound, {0, "match not found"}};
        }
        if (fetch_state == MYSQL_DATA_TRUNCATED)
        {
            return {SettlementStates::DataError, {0, "matches row was truncated"}};
        }
        if (fetch_state != 0)
        {
            return statement_failure(statement.get());
        }
        if (state_is_null || state_error || state > 1)
        {
            return {SettlementStates::DataError, {0, "invalid matches row"}};
        }
        if (state == 1)
        {
            return {SettlementStates::MatchAlreadySettled, {0, "match already settled"}};
        }
        return {};
    }

    // 验证本局恰有两名参与者，且集合等于请求中的胜者和负者。
    StepResult lock_and_validate_participants(MYSQL *connection, const ReportMatchResultRequest &request)
    {
        MYSQL_STMT *new_statement = mysql_stmt_init(connection);
        if (new_statement == nullptr)
        {
            return connection_failure(connection);
        }
        MysqlStatement statement(new_statement);
        if (mysql_stmt_prepare(statement.get(), kLockParticipantsSql, static_cast<unsigned long>(std::strlen(kLockParticipantsSql))) != 0)
        {
            return statement_failure(statement.get());
        }
        std::string bound_match_id = request.match_id;
        unsigned long match_id_length{0};
        MYSQL_BIND parameter_bind[1]{};
        bind_text(parameter_bind[0], bound_match_id, match_id_length);
        if (mysql_stmt_bind_param(statement.get(), parameter_bind) != 0)
        {
            return statement_failure(statement.get());
        }
        if (mysql_stmt_execute(statement.get()) != 0)
        {
            return statement_failure(statement.get());
        }
        MysqlStatementResult result(statement.get());
        unsigned long long player_id = 0;
        bool player_id_is_null = false;
        bool player_id_error = false;
        MYSQL_BIND result_bind[1]{};
        result_bind[0].buffer_type = MYSQL_TYPE_LONGLONG;
        result_bind[0].buffer = &player_id;
        result_bind[0].is_unsigned = true;
        result_bind[0].is_null = &player_id_is_null;
        result_bind[0].error = &player_id_error;
        if (mysql_stmt_bind_result(statement.get(), result_bind) != 0)
        {
            return statement_failure(statement.get());
        }
        std::array<std::uint64_t, 2> participants{};
        std::size_t participant_count = 0;
        while (1)
        {
            const int fetch_state = mysql_stmt_fetch(statement.get());
            if (fetch_state == MYSQL_NO_DATA)
            {
                break;
            }
            if (fetch_state == MYSQL_DATA_TRUNCATED)
            {
                return {SettlementStates::DataError, {0, "match_players.player_id was truncated"}};
            }
            if (fetch_state != 0)
            {
                return statement_failure(statement.get());
            }
            if (player_id_is_null || player_id_error || player_id == 0)
            {
                return {SettlementStates::DataError, {0, "invalid match_players.player_id"}};
            }
            if (participant_count >= participants.size())
            {
                return {SettlementStates::ParticipantMismatch, {}};
            }
            participants[participant_count] = static_cast<std::uint64_t>(player_id);
            ++participant_count;
        }
        if (participant_count != 2)
        {
            return {SettlementStates::ParticipantMismatch, {}};
        }
        const std::uint64_t expected_first = std::min(request.winner_player_id, request.loser_player_id);
        const std::uint64_t expected_second = std::max(request.winner_player_id, request.loser_player_id);
        if (participants[0] != expected_first || participants[1] != expected_second)
        {
            return {SettlementStates::ParticipantMismatch, {}};
        }
        return {};
    }

    // 查询并锁定一名玩家；不存在表示持久数据不变量被破坏。
    StepResult lock_player_for_update(MYSQL *connection, std::uint64_t player_id)
    {
        if (player_id == 0)
        {
            return {SettlementStates::DataError, {0, "invalid player_id"}};
        }
        MYSQL_STMT *new_statement = mysql_stmt_init(connection);
        if (new_statement == nullptr)
        {
            return connection_failure(connection);
        }
        MysqlStatement statement(new_statement);
        if (mysql_stmt_prepare(statement.get(), kLockPlayerSql, static_cast<unsigned long>(std::strlen(kLockPlayerSql))) != 0)
        {
            return statement_failure(statement.get());
        }
        unsigned long long bound_player_id = static_cast<unsigned long long>(player_id);
        MYSQL_BIND parameter_bind[1]{};
        bind_unsigned_long_long(parameter_bind[0], bound_player_id);
        if (mysql_stmt_bind_param(statement.get(), parameter_bind) != 0)
        {
            return statement_failure(statement.get());
        }
        if (mysql_stmt_execute(statement.get()) != 0)
        {
            return statement_failure(statement.get());
        }
        MysqlStatementResult result(statement.get());
        unsigned long long locked_player_id = 0;
        bool player_id_is_null = false;
        bool player_id_error = false;
        MYSQL_BIND result_bind[1]{};
        result_bind[0].buffer_type = MYSQL_TYPE_LONGLONG;
        result_bind[0].buffer = &locked_player_id;
        result_bind[0].is_unsigned = true;
        result_bind[0].is_null = &player_id_is_null;
        result_bind[0].error = &player_id_error;
        if (mysql_stmt_bind_result(statement.get(), result_bind) != 0)
        {
            return statement_failure(statement.get());
        }
        const int fetch_state = mysql_stmt_fetch(statement.get());
        if (fetch_state == MYSQL_NO_DATA)
        {
            return {SettlementStates::DataError, {0, "player not found"}};
        }
        if (fetch_state == MYSQL_DATA_TRUNCATED)
        {
            return {SettlementStates::DataError, {0, "players.player_id was truncated"}};
        }
        if (fetch_state != 0)
        {
            return statement_failure(statement.get());
        }
        if (player_id_is_null || player_id_error || locked_player_id == 0 || locked_player_id != bound_player_id)
        {
            return {SettlementStates::DataError, {0, "invalid players.player_id"}};
        }
        return {};
    }

    // 按 player_id 从小到大的固定顺序依次锁定两名玩家。
    StepResult lock_players_in_order(MYSQL *connection, std::uint64_t player_a, std::uint64_t player_b)
    {
        if (player_a == 0 || player_b == 0 || player_a == player_b)
        {
            return {SettlementStates::DataError, {0, "invalid player ids"}};
        }
        const std::uint64_t first_player_id = std::min(player_a, player_b);
        const std::uint64_t second_player_id = std::max(player_a, player_b);
        StepResult step = lock_player_for_update(connection, first_player_id);
        if (!step.ok())
        {
            return step;
        }
        return lock_player_for_update(connection, second_player_id);
    }

    // 把本次结算请求写入 result_requests，request_id 为主键。
    StepResult insert_result_request(MYSQL *connection, const ReportMatchResultRequest &request)
    {
        std::string request_id = request.request_id;
        std::string match_id = request.match_id;
        unsigned long request_id_length = 0;
        unsigned long match_id_length = 0;
        unsigned long long winner_player_id = static_cast<unsigned long long>(request.winner_player_id);
        unsigned long long loser_player_id = static_cast<unsigned long long>(request.loser_player_id);
        MYSQL_BIND bind[4]{};
        bind_text(bind[0], request_id, request_id_length);
        bind_text(bind[1], match_id, match_id_length);
        bind_unsigned_long_long(bind[2], winner_player_id);
        bind_unsigned_long_long(bind[3], loser_player_id);
        return execute_command(connection, kInsertResultRequestSql, bind, 1, SettlementStates::DataError);
    }

    // 给胜者累加胜场。
    StepResult increment_winner(MYSQL *connection, std::uint64_t player_id)
    {
        unsigned long long bound_player_id = static_cast<unsigned long long>(player_id);
        MYSQL_BIND bind[1]{};
        bind_unsigned_long_long(bind[0], bound_player_id);
        return execute_command(connection, kIncrementWinnerSql, bind, 1, SettlementStates::DataError);
    }

    // 给负者累加负场。
    StepResult increment_loser(MYSQL *connection, std::uint64_t player_id)
    {
        unsigned long long bound_player_id = static_cast<unsigned long long>(player_id);
        MYSQL_BIND bind[1]{};
        bind_unsigned_long_long(bind[0], bound_player_id);
        return execute_command(connection, kIncrementLoserSql, bind, 1, SettlementStates::DataError);
    }

    // 写入玩家在本局的胜负结果；影响行数异常表示事务内数据不变量被破坏。
    StepResult set_player_outcome(MYSQL *connection, const std::string &match_id, std::uint64_t player_id, unsigned char outcome)
    {
        std::string bound_match_id = match_id;
        unsigned long match_id_length = 0;
        unsigned long long bound_player_id = static_cast<unsigned long long>(player_id);
        unsigned char bound_outcome = outcome;
        MYSQL_BIND bind[3]{};
        bind_unsigned_tiny(bind[0], bound_outcome);
        bind_text(bind[1], bound_match_id, match_id_length);
        bind_unsigned_long_long(bind[2], bound_player_id);
        return execute_command(connection, kSetOutcomeSql, bind, 1, SettlementStates::DataError);
    }

    // 清除本局两名玩家的在局分配记录；玩家参数顺序不承担加锁职责。
    StepResult clear_active_assignments(MYSQL *connection, const ReportMatchResultRequest &request)
    {
        std::string bound_match_id = request.match_id;
        unsigned long match_id_length = 0;
        unsigned long long bound_winner_player_id = static_cast<unsigned long long>(request.winner_player_id);
        unsigned long long bound_loser_player_id = static_cast<unsigned long long>(request.loser_player_id);
        MYSQL_BIND bind[3]{};
        bind_text(bind[0], bound_match_id, match_id_length);
        bind_unsigned_long_long(bind[1], bound_winner_player_id);
        bind_unsigned_long_long(bind[2], bound_loser_player_id);
        return execute_command(connection, kClearActiveAssignmentsSql, bind, 2, SettlementStates::DataError);
    }

    // 把对局标记为已完成并写入完成时间。
    StepResult complete_match(MYSQL *connection, const std::string &match_id)
    {
        std::string bound_match_id = match_id;
        unsigned long match_id_length = 0;
        MYSQL_BIND bind[1]{};
        bind_text(bind[0], bound_match_id, match_id_length);
        return execute_command(connection, kCompleteMatchSql, bind, 1, SettlementStates::DataError);
    }

    // 校验请求：ID 非空且不超过 64 字节、双方 ID 均非 0 且互不相同。
    bool is_valid_request(const ReportMatchResultRequest &request) noexcept
    {
        if (request.request_id.empty() || request.match_id.empty())
        {
            return false;
        }
        if (request.request_id.size() > 64 || request.match_id.size() > 64)
        {
            return false;
        }
        if (request.winner_player_id == 0 || request.loser_player_id == 0)
        {
            return false;
        }
        if (request.winner_player_id == request.loser_player_id)
        {
            return false;
        }
        return true;
    }

    // 比较已保存结算与本次请求的完整业务 payload。
    bool same_payload(const MatchSettlement &stored, const ReportMatchResultRequest &request) noexcept
    {
        return stored.winner_player_id == request.winner_player_id && stored.loser_player_id == request.loser_player_id && stored.match_id == request.match_id;
    }

    class TransactionGuard final
    {
    public:
        // 事务已开启后构造，未提交时由析构负责回滚。
        explicit TransactionGuard(MYSQL *connection) noexcept : connection_(connection)
        {
        }

        // 若事务仍活跃则回滚，避免把未结束的事务留给调用方。
        ~TransactionGuard() noexcept
        {
            if (active_)
            {
                mysql_rollback(connection_);
            }
        }
        TransactionGuard(const TransactionGuard &) = delete;
        TransactionGuard &operator=(const TransactionGuard &) = delete;

        // 主动回滚并解除析构责任；返回回滚是否成功。
        bool rollback() noexcept
        {
            if (!active_)
            {
                return true;
            }
            active_ = false;
            return mysql_rollback(connection_) == 0;
        }

        // 先解除析构回滚责任再提交；返回 COMMIT 是否成功。
        bool commit() noexcept
        {
            /*
             * 必须先解除析构回滚责任。
             * mysql_commit() 返回失败时，可能只是 COMMIT 确认丢失，
             * 不能再由析构函数补发 ROLLBACK，把未知结果错误地当成回滚。
             */
            active_ = false;
            return mysql_commit(connection_) == 0;
        }
    private:
        MYSQL *connection_;
        bool active_{true};
    };

    // 回滚事务，并原样返回完整公开结果（错误或幂等重放）。
    ReportMatchResultResult rollback_and_return(TransactionGuard &transaction, MYSQL *connection, ReportMatchResultResult result)
    {
        if (transaction.rollback())
        {
            return result;
        }
        if (result.state == SettlementStates::ConnectionError)
        {
            return result;
        }
        const StepResult rollback_failure = connection_failure(connection);
        return {rollback_failure.state, std::nullopt, rollback_failure.error};
    }
}

// 只保存连接指针，不接管其所有权。
SettlementRepository::SettlementRepository(MYSQL *connection) noexcept : connection_(connection)
{
}

// 按“校验 -> BEGIN -> 两次请求查询及各项锁定 -> 写入 -> COMMIT/ROLLBACK”实现。
ReportMatchResultResult SettlementRepository::report_match_result(const ReportMatchResultRequest &request)
{
    return {SettlementStates::DataError, std::nullopt, {}};
}
