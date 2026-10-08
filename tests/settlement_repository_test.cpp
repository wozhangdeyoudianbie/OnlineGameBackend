#include "storage/mysql_RAII.h"
#include "storage/settlement_repository.h"

#include <gtest/gtest.h>
#include <mysql.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace
{
    class SettlementRowsCleanup final
    {
    public:
        SettlementRowsCleanup(
            MYSQL *connection,
            std::string request_id,
            std::string match_id,
            std::uint64_t winner_player_id,
            std::uint64_t loser_player_id)
            : connection_(connection),
            request_id_(std::move(request_id)),
            match_id_(std::move(match_id)),
            winner_player_id_(winner_player_id),
            loser_player_id_(loser_player_id)
        {
        }

        ~SettlementRowsCleanup() noexcept
        {
            if (connection_ == nullptr)
            {
                return;
            }

            (void)mysql_rollback(connection_);

            execute(
                "DELETE FROM result_requests WHERE request_id = '" +
                request_id_ + "'");

            execute(
                "DELETE FROM active_assignments WHERE match_id = '" +
                match_id_ + "'");

            execute(
                "DELETE FROM match_players WHERE match_id = '" +
                match_id_ + "'");

            execute(
                "DELETE FROM matches WHERE match_id = '" +
                match_id_ + "'");

            execute(
                "DELETE FROM players WHERE player_id IN (" +
                std::to_string(winner_player_id_) + ", " +
                std::to_string(loser_player_id_) + ")");
        }

        SettlementRowsCleanup(const SettlementRowsCleanup &) = delete;
        SettlementRowsCleanup &operator=(const SettlementRowsCleanup &) = delete;
        SettlementRowsCleanup(SettlementRowsCleanup &&) = delete;
        SettlementRowsCleanup &operator=(SettlementRowsCleanup &&) = delete;

    private:
        void execute(const std::string &sql) noexcept
        {
            if (mysql_query(connection_, sql.c_str()) != 0)
            {
                std::cerr << "[清理失败] "
                    << mysql_error(connection_) << '\n';
            }
        }

        MYSQL *connection_;
        std::string request_id_;
        std::string match_id_;
        std::uint64_t winner_player_id_;
        std::uint64_t loser_player_id_;
    };
}

TEST(SettlementRepositoryTest, NullConnection)
{
    SettlementRepository repository(nullptr);

    const ReportMatchResultRequest request{
        "D5-R-null",
        "D5-M-null",
        101,
        202
    };

    const auto result = repository.report_match_result(request);

    EXPECT_EQ(result.state, SettlementStates::ConnectionError);
    EXPECT_FALSE(result.settlement.has_value());
}

TEST(SettlementRepositoryTest, DatabaseRoundTripCommitsAllEffects)
{
    const char *password = std::getenv("DB_PASSWORD");
    ASSERT_NE(password, nullptr);
    ASSERT_NE(password[0], '\0');

    const char *database_name = std::getenv("DB_NAME");
    if (database_name == nullptr || database_name[0] == '\0')
    {
        database_name = "online_game_backend";
    }

    MYSQL *raw = mysql_init(nullptr);
    ASSERT_NE(raw, nullptr);
    MysqlConnection connection(raw);

    unsigned int timeout_seconds = 5U;
    ASSERT_EQ(
        mysql_options(
        raw,
        MYSQL_OPT_CONNECT_TIMEOUT,
        &timeout_seconds),
        0);

    ASSERT_NE(
        mysql_real_connect(
        raw,
        "127.0.0.1",
        "p2",
        password,
        database_name,
        13306U,
        nullptr,
        0),
        nullptr)
        << mysql_error(raw);

    const std::uint64_t seed = static_cast<std::uint64_t>(
        std::chrono::system_clock::now()
            .time_since_epoch()
            .count());

    const std::uint64_t winner_player_id = seed;
    const std::uint64_t loser_player_id = seed + 1U;
    const std::string suffix = std::to_string(seed);

    const std::string match_id = "D5-M-" + suffix;
    const std::string create_request_id = "D5-C-" + suffix;
    const std::string result_request_id = "D5-R-" + suffix;

    SettlementRowsCleanup cleanup(
        raw,
        result_request_id,
        match_id,
        winner_player_id,
        loser_player_id);

    const std::string insert_players =
        "INSERT INTO players (player_id) VALUES (" +
        std::to_string(winner_player_id) + "), (" +
        std::to_string(loser_player_id) + ")";

    ASSERT_EQ(mysql_query(raw, insert_players.c_str()), 0)
        << mysql_error(raw);

    const std::string insert_match =
        "INSERT INTO matches "
        "(match_id, create_request_id, state) VALUES ('" +
        match_id + "', '" + create_request_id + "', 0)";

    ASSERT_EQ(mysql_query(raw, insert_match.c_str()), 0)
        << mysql_error(raw);

    const std::string insert_match_players =
        "INSERT INTO match_players (match_id, player_id) VALUES ('" +
        match_id + "', " +
        std::to_string(winner_player_id) + "), ('" +
        match_id + "', " +
        std::to_string(loser_player_id) + ")";

    ASSERT_EQ(mysql_query(raw, insert_match_players.c_str()), 0)
        << mysql_error(raw);

    const std::string insert_active_assignments =
        "INSERT INTO active_assignments (match_id, player_id) VALUES ('" +
        match_id + "', " +
        std::to_string(winner_player_id) + "), ('" +
        match_id + "', " +
        std::to_string(loser_player_id) + ")";

    ASSERT_EQ(
        mysql_query(raw, insert_active_assignments.c_str()),
        0)
        << mysql_error(raw);

    SettlementRepository repository(connection.get());

    const ReportMatchResultRequest request{
        result_request_id,
        match_id,
        winner_player_id,
        loser_player_id
    };

    const auto result = repository.report_match_result(request);

    ASSERT_EQ(result.state, SettlementStates::Success)
        << result.error.message;
    ASSERT_TRUE(result.settlement.has_value());

    EXPECT_EQ(result.settlement->request_id, result_request_id);
    EXPECT_EQ(result.settlement->match_id, match_id);
    EXPECT_EQ(
        result.settlement->winner_player_id,
        winner_player_id);
    EXPECT_EQ(
        result.settlement->loser_player_id,
        loser_player_id);

    const std::string verify_sql =
        "SELECT "
        "m.state, "
        "(m.completed_at IS NOT NULL), "
        "winner.win_count, "
        "winner.loss_count, "
        "loser.win_count, "
        "loser.loss_count, "
        "winner_match.outcome, "
        "loser_match.outcome, "
        "(SELECT COUNT(*) "
        " FROM active_assignments AS active "
        " WHERE active.match_id = m.match_id), "
        "result.match_id, "
        "result.winner_player_id, "
        "result.loser_player_id "
        "FROM matches AS m "
        "JOIN players AS winner "
        "  ON winner.player_id = " +
        std::to_string(winner_player_id) + " "
        "JOIN players AS loser "
        "  ON loser.player_id = " +
        std::to_string(loser_player_id) + " "
        "JOIN match_players AS winner_match "
        "  ON winner_match.match_id = m.match_id "
        " AND winner_match.player_id = winner.player_id "
        "JOIN match_players AS loser_match "
        "  ON loser_match.match_id = m.match_id "
        " AND loser_match.player_id = loser.player_id "
        "JOIN result_requests AS result "
        "  ON result.match_id = m.match_id "
        "WHERE m.match_id = '" + match_id + "' "
        "AND result.request_id = '" + result_request_id + "'";

    ASSERT_EQ(mysql_query(raw, verify_sql.c_str()), 0)
        << mysql_error(raw);

    using MysqlResultPtr =
        std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)>;

    MysqlResultPtr query_result(
        mysql_store_result(raw),
        &mysql_free_result);

    ASSERT_NE(query_result.get(), nullptr)
        << mysql_error(raw);
    ASSERT_EQ(mysql_num_rows(query_result.get()), 1U);

    MYSQL_ROW row = mysql_fetch_row(query_result.get());
    ASSERT_NE(row, nullptr);

    for (std::size_t index = 0; index < 12U; ++index)
    {
        ASSERT_NE(row[index], nullptr);
    }

    EXPECT_STREQ(row[0], "1");
    EXPECT_STREQ(row[1], "1");
    EXPECT_STREQ(row[2], "1");
    EXPECT_STREQ(row[3], "0");
    EXPECT_STREQ(row[4], "0");
    EXPECT_STREQ(row[5], "1");
    EXPECT_STREQ(row[6], "1");
    EXPECT_STREQ(row[7], "2");
    EXPECT_STREQ(row[8], "0");
    EXPECT_STREQ(row[9], match_id.c_str());
    EXPECT_STREQ(
        row[10],
        std::to_string(winner_player_id).c_str());
    EXPECT_STREQ(
        row[11],
        std::to_string(loser_player_id).c_str());
}

int main(int argc, char **argv)
{
    if (mysql_library_init(0, nullptr, nullptr) != 0)
    {
        std::cerr << "[错误] 初始化MySQL客户端库失败\n";
        return 1;
    }

    ::testing::InitGoogleTest(&argc, argv);
    const int status = RUN_ALL_TESTS();

    mysql_library_end();
    return status;
}
