#include "storage/mysql_RAII.h"
#include <mysql.h>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace
{
    constexpr const char *kDatabaseHost = "127.0.0.1";
    constexpr const char *kDatabaseUser = "p2";
    constexpr const char *kDefaultDatabaseName = "online_game_backend";
    constexpr unsigned int kDatabasePort = 13306U;
    constexpr const char *kLookupSql = "SELECT match_id, state FROM matches WHERE create_request_id = ?";
    int statement_failure(MYSQL_STMT *statement, const char *operation)
    {
        const std::string error_message = mysql_stmt_error(statement);
        std::cerr << "[错误] " << operation << "失败：" << error_message << '\n';
        return 1;
    }
    int lookup_match(MYSQL *connection, const std::string &create_request_id)
    {
        MYSQL_STMT *raw_statement = mysql_stmt_init(connection);
        if (raw_statement == nullptr)
        {
            std::cerr << "[错误] 初始化MySQL预处理语句失败：" << mysql_error(connection) << '\n';
            return 1;
        }
        MysqlStatement statement(raw_statement);
        if (mysql_stmt_prepare(statement.get(), kLookupSql, static_cast<unsigned long>(std::strlen(kLookupSql))) != 0)
        {
            return statement_failure(statement.get(), "准备预处理语句");
        }
        std::string request_storage = create_request_id;
        unsigned long request_length = static_cast<unsigned long>(request_storage.size());
        MYSQL_BIND parameter_bind[1]{};
        parameter_bind[0].buffer_type = MYSQL_TYPE_STRING;
        parameter_bind[0].buffer = request_storage.data();
        parameter_bind[0].buffer_length = request_length;
        parameter_bind[0].length = &request_length;
        if (mysql_stmt_bind_param(statement.get(), parameter_bind) != 0)
        {
            return statement_failure(statement.get(), "绑定查询参数");
        }
        if (mysql_stmt_execute(statement.get()) != 0)
        {
            return statement_failure(statement.get(), "执行查询");
        }
        std::array<char, 64> match_id{};
        unsigned long match_id_length = 0;
        bool match_id_is_null = false;
        bool match_id_error = false;
        unsigned char state = 0;
        bool state_is_null = false;
        bool state_error = false;
        MYSQL_BIND result_bind[2]{};
        result_bind[0].buffer_type = MYSQL_TYPE_STRING;
        result_bind[0].buffer = match_id.data();
        result_bind[0].buffer_length = static_cast<unsigned long>(match_id.size());
        result_bind[0].length = &match_id_length;
        result_bind[0].is_null = &match_id_is_null;
        result_bind[0].error = &match_id_error;
        result_bind[1].buffer_type = MYSQL_TYPE_TINY;
        result_bind[1].buffer = &state;
        result_bind[1].is_unsigned = true;
        result_bind[1].is_null = &state_is_null;
        result_bind[1].error = &state_error;
        if (mysql_stmt_bind_result(statement.get(), result_bind) != 0)
        {
            return statement_failure(statement.get(), "绑定查询结果");
        }
        const int fetch_status = mysql_stmt_fetch(statement.get());
        int result = 0;
        if (fetch_status == MYSQL_NO_DATA)
        {
            std::cout << "[未找到] 创建请求 ID=" << create_request_id << '\n';
        }
        else if (fetch_status == MYSQL_DATA_TRUNCATED)
        {
            std::cerr << "[错误] MySQL返回的数据被截断\n";
            result = 1;
        }
        else if (fetch_status != 0)
        {
            return statement_failure(statement.get(), "读取查询结果");
        }
        else if (match_id_is_null || state_is_null || match_id_error || state_error || match_id_length > match_id.size())
        {
            std::cerr << "[错误] MySQL返回了无效的查询结果\n";
            result = 1;
        }
        else
        {
            const std::string match_id_value(match_id.data(), static_cast<std::size_t>(match_id_length));
            std::cout << "[找到] 对局ID=" << match_id_value << " 状态=" << static_cast<unsigned int>(state) << '\n';
        }
        return result;
    }
    int run_lookup(const char *database_password, const char *database_name, const std::string &create_request_id)
    {
        MYSQL *raw_connection = mysql_init(nullptr);
        if (raw_connection == nullptr)
        {
            std::cerr << "[错误] 初始化MySQL连接失败\n";
            return 1;
        }
        MysqlConnection connection(raw_connection);
        unsigned int connect_timeout_seconds = 5U;
        if (mysql_options(connection.get(), MYSQL_OPT_CONNECT_TIMEOUT, &connect_timeout_seconds) != 0)
        {
            const std::string error_message = mysql_error(connection.get());
            std::cerr << "[错误] 设置MySQL连接超时失败：" << error_message << '\n';
            return 1;
        }
        if (mysql_real_connect(connection.get(), kDatabaseHost, kDatabaseUser, database_password, database_name, kDatabasePort, nullptr, 0) == nullptr)
        {
            const std::string error_message = mysql_error(connection.get());
            std::cerr << "[错误] 连接MySQL失败：" << error_message << '\n';
            return 1;
        }
        return lookup_match(connection.get(), create_request_id);
    }
}

int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        std::cerr << "用法：online_game_backend_match_lookup <创建请求 ID>\n";
        return 1;
    }
    const char *database_password = std::getenv("DB_PASSWORD");
    if (database_password == nullptr || database_password[0] == '\0')
    {
        std::cerr << "[错误] 必须设置 DB_PASSWORD 环境变量\n";
        return 1;
    }
    const char *database_name = std::getenv("DB_NAME");
    if (database_name == nullptr || database_name[0] == '\0')
    {
        database_name = kDefaultDatabaseName;
    }
    if (mysql_library_init(0, nullptr, nullptr) != 0)
    {
        std::cerr << "[错误] 初始化MySQL客户端库失败\n";
        return 1;
    }
    const int result = run_lookup(database_password, database_name, argv[1]);
    mysql_library_end();
    return result;
}
