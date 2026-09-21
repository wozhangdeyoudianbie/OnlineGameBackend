#include "mysql_RAII.h"
#include<iostream>

MysqlConnection::MysqlConnection(MYSQL *connection) noexcept : connection_(connection)
{
}

MysqlConnection::~MysqlConnection() noexcept
{
    if (connection_ != nullptr)
    {
        mysql_close(connection_);
    }
}

MYSQL *MysqlConnection::get() const noexcept
{
    return connection_;
}

MysqlStatement::MysqlStatement(MYSQL_STMT *statement) noexcept : statement_(statement)
{
}

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

MYSQL_STMT *MysqlStatement::get() const noexcept
{
    return statement_;
}

MysqlStatementResult::MysqlStatementResult(MYSQL_STMT *statement) noexcept : statement_(statement)
{
}

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
