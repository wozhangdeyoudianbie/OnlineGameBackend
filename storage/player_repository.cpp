#include "player_repository.h"
#include "mysql_RAII.h"
#include <mysql.h>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace
{
    constexpr const char *kInsertPlayerSql = "INSERT INTO players (player_id) VALUES (?)";
    bool is_disconnect_error(unsigned int error_code) noexcept
    {
        return error_code == CR_SERVER_GONE_ERROR || error_code == CR_SERVER_LOST;
    }
    RepositoryResult statement_failure(MYSQL_STMT *statement)
    {
        const unsigned int error_code = mysql_stmt_errno(statement);
        const RepositoryStates state = is_disconnect_error(error_code) ? RepositoryStates::ConnectionError : RepositoryStates::SqlError;
        return {state, {error_code, mysql_stmt_error(statement)}};
    }
    constexpr const char *kGetPlayerSummarySql = "SELECT p.player_id, p.created_at, a.match_id "
        "FROM players AS p "
        "LEFT JOIN active_assignments AS a ON a.player_id = p.player_id "
        "WHERE p.player_id = ?";
    constexpr const char *kGetMatchHistorySql =
        "SELECT m.match_id, m.state, m.created_at, m.completed_at "
        "FROM match_players AS mp "
        "JOIN matches AS m ON m.match_id = mp.match_id "
        "WHERE mp.player_id = ? "
        "ORDER BY m.created_at DESC "
        "LIMIT 20";

}

PlayerRepository::PlayerRepository(MYSQL *connection) noexcept : connection_(connection)
{
}

RepositoryResult PlayerRepository::create_player(std::uint64_t player_id)
{

    if (connection_ == nullptr)
    {
        return {RepositoryStates::ConnectionError, {}};
    }
    MYSQL_STMT *new_statement = mysql_stmt_init(connection_);
    if (new_statement == nullptr)
    {
        return {RepositoryStates::SqlError, {mysql_errno(connection_), mysql_error(connection_)}};
    }
    MysqlStatement statement(new_statement);
    if (mysql_stmt_prepare(statement.get(), kInsertPlayerSql, static_cast<unsigned long>(std::strlen(kInsertPlayerSql))) != 0)
    {
        return statement_failure(statement.get());
    }
    unsigned long long bound_id = player_id;
    MYSQL_BIND bind[1]{};
    bind[0].buffer_type = MYSQL_TYPE_LONGLONG;
    bind[0].buffer = &bound_id;
    bind[0].is_unsigned = true;
    if (mysql_stmt_bind_param(statement.get(), bind) != 0)
    {
        return statement_failure(statement.get());
    }
    if (mysql_stmt_execute(statement.get()) != 0)
    {
        return statement_failure(statement.get());
    }
    return {RepositoryStates::Success, {}};
}

PlayerSummaryResult PlayerRepository::get_player_summary(std::uint64_t player_id)
{
    if (connection_ == nullptr)
    {
        return {RepositoryStates::ConnectionError, std::nullopt, {}};
    }
    MYSQL_STMT *new_statement = mysql_stmt_init(connection_);
    if (new_statement == nullptr)
    {
        return {RepositoryStates::SqlError, std::nullopt, {mysql_errno(connection_), mysql_error(connection_)}};
    }
    MysqlStatement statement(new_statement);
    if (mysql_stmt_prepare(statement.get(), kGetPlayerSummarySql, static_cast<unsigned long>(std::strlen(kGetPlayerSummarySql))) != 0)
    {
        const unsigned int error_code = mysql_stmt_errno(statement.get());
        const RepositoryStates state = is_disconnect_error(error_code) ? RepositoryStates::ConnectionError : RepositoryStates::SqlError;
        return {state, std::nullopt, {error_code, mysql_stmt_error(statement.get())}};
    }
    unsigned long long bound_id = player_id;
    MYSQL_BIND bind[1]{};
    bind[0].buffer_type = MYSQL_TYPE_LONGLONG;
    bind[0].buffer = &bound_id;
    bind[0].is_unsigned = true;
    if (mysql_stmt_bind_param(statement.get(), bind) != 0)
    {
        const unsigned int error_code = mysql_stmt_errno(statement.get());
        const RepositoryStates state = is_disconnect_error(error_code) ? RepositoryStates::ConnectionError : RepositoryStates::SqlError;
        return {state, std::nullopt, {error_code, mysql_stmt_error(statement.get())}};
    }
    if (mysql_stmt_execute(statement.get()) != 0)
    {
        const unsigned int error_code = mysql_stmt_errno(statement.get());
        const RepositoryStates state = is_disconnect_error(error_code) ? RepositoryStates::ConnectionError : RepositoryStates::SqlError;
        return {state, std::nullopt, {error_code, mysql_stmt_error(statement.get())}};
    }
    MysqlStatementResult result(statement.get());
    unsigned long long bound_player_id = 0;
    bool player_id_is_null = false;
    bool player_id_error = false;
    std::array<char, 32> created_at_buffer{};
    unsigned long created_at_length = 0;
    bool created_at_is_null = false;
    bool created_at_error = false;
    std::array<char, 64> match_id_buffer{};
    unsigned long match_id_length = 0;
    bool match_id_is_null = false;
    bool match_id_error = false;
    MYSQL_BIND result_bind[3]{};
    result_bind[0].buffer_type = MYSQL_TYPE_LONGLONG;
    result_bind[0].buffer = &bound_player_id;
    result_bind[0].is_unsigned = true;
    result_bind[0].is_null = &player_id_is_null;
    result_bind[0].error = &player_id_error;
    result_bind[1].buffer_type = MYSQL_TYPE_STRING;
    result_bind[1].buffer = created_at_buffer.data();
    result_bind[1].buffer_length = static_cast<unsigned long>(created_at_buffer.size());
    result_bind[1].length = &created_at_length;
    result_bind[1].is_null = &created_at_is_null;
    result_bind[1].error = &created_at_error;
    result_bind[2].buffer_type = MYSQL_TYPE_STRING;
    result_bind[2].buffer = match_id_buffer.data();
    result_bind[2].buffer_length = static_cast<unsigned long>(match_id_buffer.size());
    result_bind[2].length = &match_id_length;
    result_bind[2].is_null = &match_id_is_null;
    result_bind[2].error = &match_id_error;
    if (mysql_stmt_bind_result(statement.get(), result_bind) != 0)
    {
        const unsigned int error_code = mysql_stmt_errno(statement.get());
        const RepositoryStates state = is_disconnect_error(error_code) ? RepositoryStates::ConnectionError : RepositoryStates::SqlError;
        return {state, std::nullopt, {error_code, mysql_stmt_error(statement.get())}};
    }
    const int fetch_states = mysql_stmt_fetch(statement.get());
    if (fetch_states == MYSQL_NO_DATA)
    {
        return {RepositoryStates::NotFound, std::nullopt, {}};
    }
    if (fetch_states == MYSQL_DATA_TRUNCATED)
    {
        return {RepositoryStates::DataError, std::nullopt, {}};
    }
    if (fetch_states != 0)
    {
        const unsigned int error_code = mysql_stmt_errno(statement.get());
        const RepositoryStates state = is_disconnect_error(error_code) ? RepositoryStates::ConnectionError : RepositoryStates::SqlError;
        return {state, std::nullopt, {error_code, mysql_stmt_error(statement.get())}};
    }
    if (player_id_is_null || player_id_error || created_at_is_null || created_at_error || created_at_length > created_at_buffer.size() ||
    match_id_error || match_id_length > match_id_buffer.size())
    {
        return {RepositoryStates::DataError, std::nullopt, {}};
    }
    PlayerSummary summary{};
    summary.player_id = bound_player_id;
    summary.created_at = std::string(created_at_buffer.data(), static_cast<std::size_t>(created_at_length));
    if (!match_id_is_null)
    {
        summary.active_match_id = std::string(match_id_buffer.data(), static_cast<std::size_t>(match_id_length));
    }
    return {RepositoryStates::Success, summary, {}};
}

MatchHistoryResult PlayerRepository::get_match_history(std::uint64_t player_id)
{
    if (connection_ == nullptr)
    {
        return {RepositoryStates::ConnectionError, {}, {}};
    }
    MYSQL_STMT *new_statement = mysql_stmt_init(connection_);
    if (new_statement == nullptr)
    {
        return {RepositoryStates::SqlError, {}, {mysql_errno(connection_), mysql_error(connection_)}};
    }
    MysqlStatement statement(new_statement);
    if (mysql_stmt_prepare(statement.get(), kGetMatchHistorySql, static_cast<unsigned long>(std::strlen(kGetMatchHistorySql))) != 0)
    {
        const unsigned int error_code = mysql_stmt_errno(statement.get());
        const RepositoryStates state = is_disconnect_error(error_code) ? RepositoryStates::ConnectionError : RepositoryStates::SqlError;
        return {state, {}, {error_code, mysql_stmt_error(statement.get())}};
    }
    unsigned long long bound_id = player_id;
    MYSQL_BIND bind[1]{};
    bind[0].buffer_type = MYSQL_TYPE_LONGLONG;
    bind[0].buffer = &bound_id;
    bind[0].is_unsigned = true;
    if (mysql_stmt_bind_param(statement.get(), bind) != 0)
    {
        const unsigned int error_code = mysql_stmt_errno(statement.get());
        const RepositoryStates state = is_disconnect_error(error_code) ? RepositoryStates::ConnectionError : RepositoryStates::SqlError;
        return {state, {}, {error_code, mysql_stmt_error(statement.get())}};
    }
    if (mysql_stmt_execute(statement.get()) != 0)
    {
        const unsigned int error_code = mysql_stmt_errno(statement.get());
        const RepositoryStates state = is_disconnect_error(error_code) ? RepositoryStates::ConnectionError : RepositoryStates::SqlError;
        return {state, {}, {error_code, mysql_stmt_error(statement.get())}};
    }
    MysqlStatementResult result(statement.get());
    std::array<char, 64> match_id_buffer{};
    unsigned long match_id_length = 0;
    bool match_id_is_null = false;
    bool match_id_error = false;
    unsigned char bound_state = 0;
    bool state_is_null = false;
    bool state_error = false;
    std::array<char, 32> created_at_buffer{};
    unsigned long created_at_length = 0;
    bool created_at_is_null = false;
    bool created_at_error = false;
    std::array<char, 32> completed_at_buffer{};
    unsigned long completed_at_length = 0;
    bool completed_at_is_null = false;
    bool completed_at_error = false;
    MYSQL_BIND result_bind[4]{};
    result_bind[0].buffer_type = MYSQL_TYPE_STRING;
    result_bind[0].buffer = match_id_buffer.data();
    result_bind[0].buffer_length = static_cast<unsigned long>(match_id_buffer.size());
    result_bind[0].length = &match_id_length;
    result_bind[0].is_null = &match_id_is_null;
    result_bind[0].error = &match_id_error;
    result_bind[1].buffer_type = MYSQL_TYPE_TINY;
    result_bind[1].buffer = &bound_state;
    result_bind[1].is_unsigned = true;
    result_bind[1].is_null = &state_is_null;
    result_bind[1].error = &state_error;
    result_bind[2].buffer_type = MYSQL_TYPE_STRING;
    result_bind[2].buffer = created_at_buffer.data();
    result_bind[2].buffer_length = static_cast<unsigned long>(created_at_buffer.size());
    result_bind[2].length = &created_at_length;
    result_bind[2].is_null = &created_at_is_null;
    result_bind[2].error = &created_at_error;
    result_bind[3].buffer_type = MYSQL_TYPE_STRING;
    result_bind[3].buffer = completed_at_buffer.data();
    result_bind[3].buffer_length = static_cast<unsigned long>(completed_at_buffer.size());
    result_bind[3].length = &completed_at_length;
    result_bind[3].is_null = &completed_at_is_null;
    result_bind[3].error = &completed_at_error;
    if (mysql_stmt_bind_result(statement.get(), result_bind) != 0)
    {
        const unsigned int error_code = mysql_stmt_errno(statement.get());
        const RepositoryStates state = is_disconnect_error(error_code) ? RepositoryStates::ConnectionError : RepositoryStates::SqlError;
        return {state, {}, {error_code, mysql_stmt_error(statement.get())}};
    }
    std::vector<MatchHistoryItem> matches;
    while (1)
    {
        const int fetch_status = mysql_stmt_fetch(statement.get());
        if (fetch_status == MYSQL_NO_DATA)
        {
            break;
        }
        if (fetch_status == MYSQL_DATA_TRUNCATED)
        {
            return {RepositoryStates::DataError, {}, {}};
        }
        if (fetch_status != 0)
        {
            const unsigned int error_code = mysql_stmt_errno(statement.get());
            const RepositoryStates state = is_disconnect_error(error_code) ? RepositoryStates::ConnectionError : RepositoryStates::SqlError;
            return {state, {}, {error_code, mysql_stmt_error(statement.get())}};
        }
        if (match_id_is_null || match_id_error || match_id_length > match_id_buffer.size() || state_is_null || state_error
            || created_at_is_null || created_at_error || created_at_length > created_at_buffer.size()
            || completed_at_error || completed_at_length > completed_at_buffer.size())
        {
            return {RepositoryStates::DataError, {}, {}};
        }
        MatchHistoryItem item{};
        item.match_id = std::string(match_id_buffer.data(), static_cast<std::size_t>(match_id_length));
        item.state = bound_state;
        item.created_at = std::string(created_at_buffer.data(), static_cast<std::size_t>(created_at_length));
        if (!completed_at_is_null)
        {
            item.completed_at = std::string(completed_at_buffer.data(), static_cast<std::size_t>(completed_at_length));
        }
        matches.push_back(item);
    }
    return {RepositoryStates::Success, std::move(matches), {}};
}
