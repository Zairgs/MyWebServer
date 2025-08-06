#include <mysql/mysql.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <list>
#include <pthread.h>
#include <iostream>
#include "sql_connection_pool.h"

using namespace std;

connection_pool::connection_pool() // 构造函数
{
    m_CurConn = 0;  // 当前已使用的连接数
    m_FreeConn = 0; // 当前空闲的连接数
}

connection_pool *connection_pool::GetInstance() // 获取连接池的实例
{
    static connection_pool connPool; // 静态变量，只创建一次
    return &connPool;                // 返回连接池的实例
}

void connection_pool::init(string url, string User, string PassWord, string DataBaseName, int Port, int MaxConn, int close_log) // 初始化数据库信息
{
    m_url = url;                   // 数据库地址
    m_Port = Port;                 // 数据库端口
    m_User = User;                 // 数据库用户名
    m_PassWord = PassWord;         // 数据库密码
    m_DatabaseName = DataBaseName; // 数据库名
    m_close_log = close_log;       // 日志开关

    for (int i = 0; i < MaxConn; i++) // 创建MaxConn个数据库连接
    {
        MYSQL *con = NULL;     // 创建MYSQL指针
        con = mysql_init(con); // 初始化MYSQL指针
        if (con == NULL)
        {
            LOG_ERROR("MySQL Error");
            exit(1);
        }
        con = mysql_real_connect(con, m_url.c_str(), m_User.c_str(), m_PassWord.c_str(), m_DatabaseName.c_str(), atoi(m_Port.c_str()), NULL, 0);

        if (con == NULL)
        {
            LOG_ERROR("MySQL Error");
            exit(1);
        }
        connList.push_back(con); // 将MYSQL指针添加到连接池中
        ++m_FreeConn;              // 当前空闲的连接数加1
    }

    reserve = sem(m_FreeConn); // 信号量初始化

    m_MaxConn = m_FreeConn; // 最大连接数
}

MYSQL *connection_pool::GetConnection() // 当有请求时，从数据库连接池中返回一个可用连接，更新使用和空闲连接数
{
    MYSQL *con = NULL;

    if (0 == m_FreeConn) // 如果当前没有空闲的连接
    {
        // LOG_WARN("连接池没有空闲连接");
        return NULL;
    }

    reserve.wait(); // 等待信号量

    lock.lock(); // 上锁

    con = connList.front(); // 获取连接池中的第一个连接
    connList.pop_front();     // 删除连接池中的第一个连接

    --m_FreeConn; // 当前空闲的连接数减1
    ++m_CurConn;  // 当前使用的连接数加1

    lock.unlock();
    return con;
}

bool connection_pool::ReleaseConnection(MYSQL *conn) // 释放当前使用的连接
{
    if (NULL == conn)
        return false;

    lock.lock(); // 上锁

    connList.push_back(conn); // 将连接放回连接池
    ++m_FreeConn;               // 当前空闲的连接数加1
    --m_CurConn;                // 当前使用的连接数减1

    lock.unlock();

    reserve.post(); // 信号量加1

    return true;
}

void connection_pool::DestroyPool() // 销毁数据库连接池
{
    lock.lock();
	if (connList.size() > 0)
	{
		list<MYSQL *>::iterator it;
		for (it = connList.begin(); it != connList.end(); ++it)
		{
			MYSQL *con = *it;
			mysql_close(con);
		}
		m_CurConn = 0;
		m_FreeConn = 0;
		connList.clear();
	}

	lock.unlock();
}

int connection_pool::GetFreeConn() // 获取当前空闲的连接数
{
    return this->m_FreeConn;
}

connection_pool::~connection_pool() // 析构函数
{
    DestroyPool();
}

connectionRAII::connectionRAII(MYSQL **SQL, connection_pool *connPool) // 构造函数
{
    *SQL = connPool->GetConnection();

    conRAII = *SQL;
    poolRAII = connPool;
}

connectionRAII::~connectionRAII() // 析构函数
{
    poolRAII->ReleaseConnection(conRAII);
}
