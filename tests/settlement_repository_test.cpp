#include "storage/mysql_RAII.h"
#include "storage/settlement_repository.h"

#include <gtest/gtest.h>
#include <mysql.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace
{
    class SettlementRowsCleanup final
    {
    public:
        SettlementRowsCleanup(MYSQL *connection, std::string match_id, std::uint64_t winner_player_id, std::uint64_t loser_player_id)
            : connection_(connection), match_id_(std::move(match_id)), winner_player_id_(winner_player_id), loser_player_id_(loser_player_id)
        {
        }

        ~SettlementRowsCleanup() noexcept
        {
            if (connection_ == nullptr)
            {
                return;
            }

            (void)mysql_rollback(connection_);

            execute("DELETE FROM result_requests WHERE match_id = '" + match_id_ + "'");

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

TEST(SettlementRepositoryTest, DatabaseRoundTripEnforcesIdempotentSettlement)
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

    MYSQL *second_raw = mysql_init(nullptr);
    ASSERT_NE(second_raw, nullptr);
    MysqlConnection second_connection(second_raw);
    ASSERT_NE(mysql_real_connect(second_raw, "127.0.0.1", "p2", password, database_name, 13306U, nullptr, 0), nullptr) << mysql_error(second_raw);

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

    SettlementRowsCleanup cleanup(raw, match_id, winner_player_id, loser_player_id);

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

    SettlementRepository repository(raw);
    SettlementRepository second_repository(second_raw);
    const ReportMatchResultRequest request{result_request_id, match_id, winner_player_id, loser_player_id};
    const ReportMatchResultRequest competing_request{"D5-R2-" + suffix, match_id, winner_player_id, loser_player_id};

    ReportMatchResultResult first_result;
    ReportMatchResultResult second_result;
    int first_thread_init = 1;
    int second_thread_init = 1;
    std::mutex start_mutex;
    std::condition_variable start_condition;
    std::size_t ready_count = 0;
    bool start = false;

    auto invoke = [&](SettlementRepository &target_repository, const ReportMatchResultRequest &target_request, ReportMatchResultResult &target_result, int &thread_init)
    {
        thread_init = mysql_thread_init();
        {
            std::unique_lock<std::mutex> lock(start_mutex);
            ++ready_count;
            start_condition.notify_all();
            start_condition.wait(lock, [&] { return start; });
        }
        if (thread_init == 0)
        {
            target_result = target_repository.report_match_result(target_request);
            mysql_thread_end();
        }
    };

    std::thread first_thread([&]()
    {
        invoke(repository, request, first_result, first_thread_init);
    });
    std::thread second_thread([&]()
    {
        invoke(second_repository, competing_request, second_result, second_thread_init);
    });

    {
        std::unique_lock<std::mutex> lock(start_mutex);
        start_condition.wait(lock, [&] { return ready_count == 2; });
        start = true;
    }
    start_condition.notify_all();

    first_thread.join();
    second_thread.join();

    ASSERT_EQ(first_thread_init, 0);
    ASSERT_EQ(second_thread_init, 0);

    const bool first_succeeded = first_result.state == SettlementStates::Success;
    const bool second_succeeded = second_result.state == SettlementStates::Success;
    ASSERT_EQ(static_cast<int>(first_succeeded) + static_cast<int>(second_succeeded), 1)
        << "first=" << static_cast<int>(first_result.state)
        << " second=" << static_cast<int>(second_result.state);

    const ReportMatchResultResult &result = first_succeeded ? first_result : second_result;
    const ReportMatchResultResult &losing_result = first_succeeded ? second_result : first_result;
    const ReportMatchResultRequest &committed_request = first_succeeded ? request : competing_request;
    const ReportMatchResultRequest &losing_request = first_succeeded ? competing_request : request;

    EXPECT_TRUE(losing_result.state == SettlementStates::MatchAlreadySettled || losing_result.state == SettlementStates::RetryableError)
        << "losing state=" << static_cast<int>(losing_result.state)
        << " error=" << losing_result.error.message;

    if (losing_result.state == SettlementStates::RetryableError)
    {
        const auto retry_result = repository.report_match_result(losing_request);
        EXPECT_EQ(retry_result.state, SettlementStates::MatchAlreadySettled) << retry_result.error.message;
    }

    ASSERT_EQ(result.state, SettlementStates::Success)
        << result.error.message;
    ASSERT_TRUE(result.settlement.has_value());

    EXPECT_EQ(result.settlement->request_id, committed_request.request_id);
    EXPECT_EQ(result.settlement->match_id, match_id);
    EXPECT_EQ(
        result.settlement->winner_player_id,
        winner_player_id);
    EXPECT_EQ(
        result.settlement->loser_player_id,
        loser_player_id);

    const auto replay_result = repository.report_match_result(committed_request);

    ASSERT_EQ(replay_result.state, SettlementStates::Replayed) << replay_result.error.message;
    ASSERT_TRUE(replay_result.settlement.has_value());
    EXPECT_EQ(replay_result.settlement->request_id, committed_request.request_id);
    EXPECT_EQ(replay_result.settlement->match_id, match_id);
    EXPECT_EQ(replay_result.settlement->winner_player_id, winner_player_id);
    EXPECT_EQ(replay_result.settlement->loser_player_id, loser_player_id);

    const ReportMatchResultRequest conflict_request{committed_request.request_id, match_id, loser_player_id, winner_player_id};
    const auto conflict_result = repository.report_match_result(conflict_request);

    EXPECT_EQ(conflict_result.state, SettlementStates::RequestConflict) << conflict_result.error.message;
    EXPECT_FALSE(conflict_result.settlement.has_value());

    const ReportMatchResultRequest already_settled_request{"D5-R3-" + suffix, match_id, winner_player_id, loser_player_id};
    const auto already_settled_result = repository.report_match_result(already_settled_request);

    EXPECT_EQ(already_settled_result.state, SettlementStates::MatchAlreadySettled) << already_settled_result.error.message;
    EXPECT_FALSE(already_settled_result.settlement.has_value());

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
        "(SELECT COUNT(*) FROM result_requests AS requests "
        " WHERE requests.match_id = m.match_id), "
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
        "AND result.request_id = '" + committed_request.request_id + "'";

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

    for (std::size_t index = 0; index < 13U; ++index)
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
    EXPECT_STREQ(row[9], "1");
    EXPECT_STREQ(row[10], match_id.c_str());
    EXPECT_STREQ(
        row[11],
        std::to_string(winner_player_id).c_str());
    EXPECT_STREQ(
        row[12],
        std::to_string(loser_player_id).c_str());
}

TEST(SettlementRepositoryTest, ConcurrentSameRequestReplaysOrRetriesWithoutDuplicateEffects)
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

    MYSQL *second_raw = mysql_init(nullptr);
    ASSERT_NE(second_raw, nullptr);
    MysqlConnection second_connection(second_raw);
    ASSERT_NE(mysql_real_connect(second_raw, "127.0.0.1", "p2", password, database_name, 13306U, nullptr, 0), nullptr) << mysql_error(second_raw);

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

    SettlementRowsCleanup cleanup(raw, match_id, winner_player_id, loser_player_id);

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

    SettlementRepository repository(raw);
    SettlementRepository second_repository(second_raw);
    const ReportMatchResultRequest request{result_request_id, match_id, winner_player_id, loser_player_id};
    const ReportMatchResultRequest competing_request = request;

    ReportMatchResultResult first_result;
    ReportMatchResultResult second_result;
    int first_thread_init = 1;
    int second_thread_init = 1;
    std::mutex start_mutex;
    std::condition_variable start_condition;
    std::size_t ready_count = 0;
    bool start = false;

    auto invoke = [&](SettlementRepository &target_repository, const ReportMatchResultRequest &target_request, ReportMatchResultResult &target_result, int &thread_init)
    {
        thread_init = mysql_thread_init();
        {
            std::unique_lock<std::mutex> lock(start_mutex);
            ++ready_count;
            start_condition.notify_all();
            start_condition.wait(lock, [&] { return start; });
        }
        if (thread_init == 0)
        {
            target_result = target_repository.report_match_result(target_request);
            mysql_thread_end();
        }
    };

    std::thread first_thread([&]()
    {
        invoke(repository, request, first_result, first_thread_init);
    });
    std::thread second_thread([&]()
    {
        invoke(second_repository, competing_request, second_result, second_thread_init);
    });

    {
        std::unique_lock<std::mutex> lock(start_mutex);
        start_condition.wait(lock, [&] { return ready_count == 2; });
        start = true;
    }
    start_condition.notify_all();

    first_thread.join();
    second_thread.join();

    ASSERT_EQ(first_thread_init, 0);
    ASSERT_EQ(second_thread_init, 0);

    const bool first_succeeded = first_result.state == SettlementStates::Success;
    const bool second_succeeded = second_result.state == SettlementStates::Success;
    ASSERT_EQ(static_cast<int>(first_succeeded) + static_cast<int>(second_succeeded), 1)
        << "first=" << static_cast<int>(first_result.state)
        << " second=" << static_cast<int>(second_result.state);

    const ReportMatchResultResult &result = first_succeeded ? first_result : second_result;
    const ReportMatchResultResult &losing_result = first_succeeded ? second_result : first_result;
    const ReportMatchResultRequest &committed_request = first_succeeded ? request : competing_request;
    const ReportMatchResultRequest &losing_request = first_succeeded ? competing_request : request;

    EXPECT_TRUE(losing_result.state == SettlementStates::Replayed || losing_result.state == SettlementStates::RetryableError)
        << "losing state=" << static_cast<int>(losing_result.state)
        << " error=" << losing_result.error.message;

    if (losing_result.state == SettlementStates::RetryableError)
    {
        const auto retry_result = repository.report_match_result(losing_request);
        ASSERT_EQ(retry_result.state, SettlementStates::Replayed) << retry_result.error.message;
        ASSERT_TRUE(retry_result.settlement.has_value());
        EXPECT_EQ(retry_result.settlement->request_id, request.request_id);
    }
    else
    {
        ASSERT_TRUE(losing_result.settlement.has_value());
        EXPECT_EQ(losing_result.settlement->request_id, request.request_id);
    }

    ASSERT_EQ(result.state, SettlementStates::Success)
        << result.error.message;
    ASSERT_TRUE(result.settlement.has_value());

    EXPECT_EQ(result.settlement->request_id, committed_request.request_id);
    EXPECT_EQ(result.settlement->match_id, match_id);
    EXPECT_EQ(
        result.settlement->winner_player_id,
        winner_player_id);
    EXPECT_EQ(
        result.settlement->loser_player_id,
        loser_player_id);

    const auto replay_result = repository.report_match_result(committed_request);

    ASSERT_EQ(replay_result.state, SettlementStates::Replayed) << replay_result.error.message;
    ASSERT_TRUE(replay_result.settlement.has_value());
    EXPECT_EQ(replay_result.settlement->request_id, committed_request.request_id);
    EXPECT_EQ(replay_result.settlement->match_id, match_id);
    EXPECT_EQ(replay_result.settlement->winner_player_id, winner_player_id);
    EXPECT_EQ(replay_result.settlement->loser_player_id, loser_player_id);

    const ReportMatchResultRequest conflict_request{committed_request.request_id, match_id, loser_player_id, winner_player_id};
    const auto conflict_result = repository.report_match_result(conflict_request);

    EXPECT_EQ(conflict_result.state, SettlementStates::RequestConflict) << conflict_result.error.message;
    EXPECT_FALSE(conflict_result.settlement.has_value());

    const ReportMatchResultRequest already_settled_request{"D5-R3-" + suffix, match_id, winner_player_id, loser_player_id};
    const auto already_settled_result = repository.report_match_result(already_settled_request);

    EXPECT_EQ(already_settled_result.state, SettlementStates::MatchAlreadySettled) << already_settled_result.error.message;
    EXPECT_FALSE(already_settled_result.settlement.has_value());

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
        "(SELECT COUNT(*) FROM result_requests AS requests "
        " WHERE requests.match_id = m.match_id), "
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
        "AND result.request_id = '" + committed_request.request_id + "'";

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

    for (std::size_t index = 0; index < 13U; ++index)
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
    EXPECT_STREQ(row[9], "1");
    EXPECT_STREQ(row[10], match_id.c_str());
    EXPECT_STREQ(
        row[11],
        std::to_string(winner_player_id).c_str());
    EXPECT_STREQ(
        row[12],
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
