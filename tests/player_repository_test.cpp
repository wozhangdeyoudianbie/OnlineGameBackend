#include "storage/mysql_RAII.h"
#include "storage/player_repository.h"

#include <gtest/gtest.h>
#include <mysql.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

TEST(PlayerRepositoryTest, NullConnection)
{
    PlayerRepository repository(nullptr);
    EXPECT_EQ(repository.create_player(1).state, RepositoryStates::ConnectionError);
    EXPECT_EQ(repository.get_player_summary(1).state, RepositoryStates::ConnectionError);
    EXPECT_EQ(repository.get_match_history(1).state, RepositoryStates::ConnectionError);
}

TEST(PlayerRepositoryTest, DatabaseRoundTrip)
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
    ASSERT_EQ(mysql_options(raw, MYSQL_OPT_CONNECT_TIMEOUT, &timeout_seconds), 0)
        << mysql_error(raw);
    ASSERT_NE(mysql_real_connect(raw, "127.0.0.1", "p2", password,
              database_name, 13306U, nullptr, 0), nullptr)
        << mysql_error(raw);
    ASSERT_EQ(mysql_query(raw, "START TRANSACTION"), 0) << mysql_error(raw);

    PlayerRepository repository(connection.get());
    const std::uint64_t player_id = static_cast<std::uint64_t>(
        std::chrono::system_clock::now().time_since_epoch().count());
    const std::string id = std::to_string(player_id);

    EXPECT_EQ(repository.get_player_summary(player_id).state, RepositoryStates::NotFound);

    const auto created = repository.create_player(player_id);
    ASSERT_EQ(created.state, RepositoryStates::Success) << created.error.message;

    const auto duplicate = repository.create_player(player_id);
    EXPECT_EQ(duplicate.state, RepositoryStates::SqlError);
    EXPECT_EQ(duplicate.error.code, 1062U) << duplicate.error.message;

    const auto initial_summary = repository.get_player_summary(player_id);
    ASSERT_EQ(initial_summary.state, RepositoryStates::Success)
        << initial_summary.error.message;
    ASSERT_TRUE(initial_summary.summary.has_value());
    EXPECT_EQ(initial_summary.summary->player_id, player_id);
    EXPECT_FALSE(initial_summary.summary->created_at.empty());
    EXPECT_FALSE(initial_summary.summary->active_match_id.has_value());

    const auto empty_history = repository.get_match_history(player_id);
    ASSERT_EQ(empty_history.state, RepositoryStates::Success)
        << empty_history.error.message;
    EXPECT_TRUE(empty_history.matches.empty());

    const std::string base = "D4-" + id;
    const std::string active_match = base + "-A";
    const std::string completed_match = std::string(64U - base.size(), 'X') + base;

    const std::string insert_matches =
        "INSERT INTO matches (match_id, create_request_id, state, created_at, completed_at) VALUES "
        "('" + active_match + "', 'RA-" + id +
        "', 0, '2026-01-01 00:00:00.000001', NULL), "
        "('" + completed_match + "', 'RB-" + id +
        "', 1, '2026-01-02 00:00:00.000001', '2026-01-03 00:00:00.000001')";
    ASSERT_EQ(mysql_query(raw, insert_matches.c_str()), 0) << mysql_error(raw);

    const std::string insert_players =
        "INSERT INTO match_players (match_id, player_id) VALUES "
        "('" + active_match + "', " + id + "), "
        "('" + completed_match + "', " + id + ")";
    ASSERT_EQ(mysql_query(raw, insert_players.c_str()), 0) << mysql_error(raw);

    const std::string insert_active =
        "INSERT INTO active_assignments (match_id, player_id) VALUES "
        "('" + active_match + "', " + id + ")";
    ASSERT_EQ(mysql_query(raw, insert_active.c_str()), 0) << mysql_error(raw);

    const auto summary = repository.get_player_summary(player_id);
    ASSERT_EQ(summary.state, RepositoryStates::Success) << summary.error.message;
    ASSERT_TRUE(summary.summary.has_value());
    ASSERT_TRUE(summary.summary->active_match_id.has_value());
    EXPECT_EQ(*summary.summary->active_match_id, active_match);

    const auto history = repository.get_match_history(player_id);
    ASSERT_EQ(history.state, RepositoryStates::Success) << history.error.message;
    ASSERT_EQ(history.matches.size(), 2U);
    EXPECT_EQ(history.matches[0].match_id, completed_match);
    EXPECT_EQ(history.matches[0].match_id.size(), 64U);
    EXPECT_EQ(history.matches[0].state, 1U);
    EXPECT_TRUE(history.matches[0].completed_at.has_value());
    EXPECT_EQ(history.matches[1].match_id, active_match);
    EXPECT_EQ(history.matches[1].state, 0U);
    EXPECT_FALSE(history.matches[1].completed_at.has_value());

    ASSERT_EQ(mysql_query(raw, "ROLLBACK"), 0) << mysql_error(raw);
    EXPECT_EQ(repository.get_player_summary(player_id).state, RepositoryStates::NotFound);
}

TEST(PlayerRepositoryTest, DisconnectedConnection)
{
    const char *password = std::getenv("DB_PASSWORD");
    ASSERT_NE(password, nullptr);
    ASSERT_NE(password[0], '\0');

    const char *database_name = std::getenv("DB_NAME");
    if (database_name == nullptr || database_name[0] == '\0')
    {
        database_name = "online_game_backend";
    }

    MYSQL *victim_raw = mysql_init(nullptr);
    ASSERT_NE(victim_raw, nullptr);
    MysqlConnection victim(victim_raw);

    MYSQL *control_raw = mysql_init(nullptr);
    ASSERT_NE(control_raw, nullptr);
    MysqlConnection control(control_raw);

    unsigned int timeout_seconds = 5U;
    ASSERT_EQ(mysql_options(victim_raw, MYSQL_OPT_CONNECT_TIMEOUT, &timeout_seconds), 0);
    ASSERT_EQ(mysql_options(control_raw, MYSQL_OPT_CONNECT_TIMEOUT, &timeout_seconds), 0);

    ASSERT_NE(mysql_real_connect(victim_raw, "127.0.0.1", "p2", password,
              database_name, 13306U, nullptr, 0), nullptr)
        << mysql_error(victim_raw);
    ASSERT_NE(mysql_real_connect(control_raw, "127.0.0.1", "p2", password,
              database_name, 13306U, nullptr, 0), nullptr)
        << mysql_error(control_raw);

    ASSERT_EQ(mysql_query(victim_raw, "SELECT CONNECTION_ID()"), 0)
        << mysql_error(victim_raw);
    MYSQL_RES *id_result = mysql_store_result(victim_raw);
    ASSERT_NE(id_result, nullptr) << mysql_error(victim_raw);
    MYSQL_ROW id_row = mysql_fetch_row(id_result);
    const std::string victim_id =
        (id_row != nullptr && id_row[0] != nullptr) ? id_row[0] : "";
    mysql_free_result(id_result);
    ASSERT_FALSE(victim_id.empty());

    const std::string kill_sql = "KILL CONNECTION " + victim_id;
    ASSERT_EQ(mysql_query(control_raw, kill_sql.c_str()), 0)
        << mysql_error(control_raw);

    bool disconnected = false;
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        if (mysql_ping(victim_raw) != 0)
        {
            disconnected = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(disconnected) << "测试连接未确认断开";

    PlayerRepository repository(victim.get());
    const auto summary = repository.get_player_summary(1);
    EXPECT_EQ(summary.state, RepositoryStates::ConnectionError)
        << summary.error.message;
    EXPECT_NE(summary.error.code, 0U);

    const auto history = repository.get_match_history(1);
    EXPECT_EQ(history.state, RepositoryStates::ConnectionError)
        << history.error.message;
    EXPECT_NE(history.error.code, 0U);
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
