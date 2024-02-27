#ifndef SERVERSIMU_SQL_CONNECTION_POOL_H
#define SERVERSIMU_SQL_CONNECTION_POOL_H
#include <stdio.h>
#include <list>
#include <mysql/mysql.h>
#include <error.h>
#include <string.h>
#include <iostream>
#include <string>
#include "../lock/locker.h"

using namespace std;

class connection_pool {
public:
    MYSQL* GetConnection();              //获取数据库连接
    bool ReleaseConnection(MYSQL *conn); //释放连接
    int GetFreeConn();					 //获取连接
    void DestroyPool();					 //销毁所有连接
    //局部静态变量单例模式
    static connection_pool *GetInstance();

    void init(string url, string User, string PassWord, string DataBaseName, int Port, unsigned int MaxConn);

    connection_pool();
    ~connection_pool();
private:
    unsigned int MaxConn;
    unsigned int CurConn;
    unsigned int FreeConn;
private:
    locker lock;
    list<MYSQL *> connList;
    sem reserve;
private:
    string url;
    string Port;
    string User;
    string PassWord;
    string DatabaseName;
};

class connectionRAII{

public:
    connectionRAII(MYSQL **con, connection_pool *connPool);
    ~connectionRAII();

private:
    MYSQL *conRAII;
    connection_pool *poolRAII;
};


#endif //SERVERSIMU_SQL_CONNECTION_POOL_H
