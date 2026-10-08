#include "mysql_RAII.h"
#include<iostream>

// 接管一个已建立的连接，析构时负责关闭。
MysqlConnection::MysqlConnection(MYSQL *connection) noexcept : connection_(connection)
{
}

// 关闭连接；句柄为空时不做任何事。
MysqlConnection::~MysqlConnection() noexcept
{
    if (connection_ != nullptr)
    {
        mysql_close(connection_);
    }
}

// 返回裸连接指针，所有权仍归本对象。
MYSQL *MysqlConnection::get() const noexcept
{
    return connection_;
}

// 接管一个已初始化的预处理语句，析构时负责关闭。
MysqlStatement::MysqlStatement(MYSQL_STMT *statement) noexcept : statement_(statement)
{
}

// 关闭预处理语句；句柄为空或关闭失败时打印错误。
MysqlStatement::~MysqlStatement() noexcept
{
    if (statement_ == nullptr)
    {
        std::cerr << "[错误] statement不存在\n";
        return;
    }
    if (mysql_stmt_close(statement_))
    {
        std::cerr << "[错误] 关闭Mysql_stmt失败\n";
    }
}

// 返回裸语句指针，所有权仍归本对象。
MYSQL_STMT *MysqlStatement::get() const noexcept
{
    return statement_;
}

// 记录需要释放结果集的语句，其生命周期必须长于本对象。
MysqlStatementResult::MysqlStatementResult(MYSQL_STMT *statement) noexcept : statement_(statement)
{
}

// 释放语句上的结果集，避免连接里残留未读完的数据。
MysqlStatementResult::~MysqlStatementResult() noexcept
{
    if (statement_ == nullptr)
    {
        std::cerr << "[错误] statement不存在\n";
        return;
    }
    if (mysql_stmt_free_result(statement_))
    {
        std::cerr << "[错误] 释放结果失败\n";
        return;
    }
}
