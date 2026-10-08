#pragma once

#include <mysql.h>

class MysqlConnection final
{
public:
    // 接管一个已建立的连接，析构时负责关闭。
    explicit MysqlConnection(MYSQL *connection) noexcept;

    // 关闭连接；句柄为空时不做任何事。
    ~MysqlConnection() noexcept;

    // 独占资源，禁止拷贝与移动。
    MysqlConnection(const MysqlConnection &) = delete;
    MysqlConnection &operator=(const MysqlConnection &) = delete;
    MysqlConnection(MysqlConnection &&) = delete;
    MysqlConnection &operator=(MysqlConnection &&) = delete;

    // 返回裸连接指针，所有权仍归本对象。
    MYSQL *get() const noexcept;
private:
    MYSQL *connection_;
};

class MysqlStatement final
{
public:
    // 接管一个已初始化的预处理语句，析构时负责关闭。
    explicit MysqlStatement(MYSQL_STMT *statement) noexcept;

    // 关闭预处理语句；句柄为空或关闭失败时打印错误。
    ~MysqlStatement() noexcept;

    // 独占资源，禁止拷贝与移动。
    MysqlStatement(const MysqlStatement &) = delete;
    MysqlStatement &operator=(const MysqlStatement &) = delete;
    MysqlStatement(MysqlStatement &&) = delete;
    MysqlStatement &operator=(MysqlStatement &&) = delete;

    // 返回裸语句指针，所有权仍归本对象。
    MYSQL_STMT *get() const noexcept;
private:
    MYSQL_STMT *statement_;
};

class MysqlStatementResult final
{
public:
    // 记录需要释放结果集的语句，其生命周期必须长于本对象。
    explicit MysqlStatementResult(MYSQL_STMT *statement) noexcept;

    // 释放语句上的结果集，避免连接里残留未读完的数据。
    ~MysqlStatementResult() noexcept;

    // 独占资源，禁止拷贝与移动。
    MysqlStatementResult(const MysqlStatementResult &) = delete;
    MysqlStatementResult &operator=(const MysqlStatementResult &) = delete;
    MysqlStatementResult(MysqlStatementResult &&) = delete;
    MysqlStatementResult &operator=(MysqlStatementResult &&) = delete;
private:
    MYSQL_STMT *statement_;
};
