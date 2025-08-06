#ifndef _CONNECTION_POOL_
#define _CONNECTION_POOL_

#include <stdio.h>
#include <list>
#include <mysql/mysql.h>
#include <error.h>
#include <string.h>
#include <iostream>
#include <string>
#include "../lock/locker.h"
#include "../log/log.h"

class connection_pool
{
public:
    MYSQL *GetConnection();
    bool ReleaseConnection(MYSQL *conn);
    int GetFreeConn();
    void DestroyPool();

    static connection_pool *GetInstance();

    void init(std::string url, std::string User, std::string PassWord, std::string DataBaseName, int Port, int MaxConn, int close_log);

private:
    connection_pool();
    ~connection_pool();

    int m_MaxConn;  // 最大连接数
    int m_CurConn;  // 当前已使用的连接数
    int m_FreeConn; // 当前空闲的连接数
    locker lock;
    list<MYSQL *> connList; // 连接池
    sem reserve;

public:
    std::string m_url;// 主机地址
    std::string m_Port;
    std::string m_User;
    std::string m_PassWord;
    std::string m_DatabaseName;
    int m_close_log;
};

class connectionRAII
{
public:
    connectionRAII(MYSQL **con, connection_pool *connPool);
    ~connectionRAII();

private:
    MYSQL *conRAII;
    connection_pool *poolRAII;
};

#endif