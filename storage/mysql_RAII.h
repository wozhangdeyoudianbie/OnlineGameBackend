#pragma once

#include <mysql.h>

class MysqlConnection final
{
public:
    explicit MysqlConnection(MYSQL *connection) noexcept;
    ~MysqlConnection() noexcept;
    MysqlConnection(const MysqlConnection &) = delete;
    MysqlConnection &operator=(const MysqlConnection &) = delete;
    MysqlConnection(MysqlConnection &&) = delete;
    MysqlConnection &operator=(MysqlConnection &&) = delete;
    MYSQL *get() const noexcept;
private:
    MYSQL *connection_;
};

class MysqlStatement final
{
public:
    explicit MysqlStatement(MYSQL_STMT *statement) noexcept;
    ~MysqlStatement() noexcept;
    MysqlStatement(const MysqlStatement &) = delete;
    MysqlStatement &operator=(const MysqlStatement &) = delete;
    MysqlStatement(MysqlStatement &&) = delete;
    MysqlStatement &operator=(MysqlStatement &&) = delete;
    MYSQL_STMT *get() const noexcept;
private:
    MYSQL_STMT *statement_;
};

class MysqlStatementResult final
{
public:
    explicit MysqlStatementResult(MYSQL_STMT *statement) noexcept;
    ~MysqlStatementResult() noexcept;
    MysqlStatementResult(const MysqlStatementResult &) = delete;
    MysqlStatementResult &operator=(const MysqlStatementResult &) = delete;
    MysqlStatementResult(MysqlStatementResult &&) = delete;
    MysqlStatementResult &operator=(MysqlStatementResult &&) = delete;
private:
    MYSQL_STMT *statement_;
};
