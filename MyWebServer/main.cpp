#include "config.h"
#include <sys/stat.h>

int main(int argc, char *argv[])
{
    string user = "root";
    string passwd = "z1234567890";
    string databasename = "yourdb";

    //命令行解析
    Config config;
    config.parse_arg(argc, argv);

    WebServer server;

    //初始化
    server.init(config.PORT, user, passwd, databasename, config.LOGWrite, 
                config.OPT_LINGER, config.TRIGMode,  config.sql_num,  config.thread_num, 
                config.close_log, config.actor_model);
    

    //日志
    server.log_write();

    //数据库
    server.sql_pool();

    //线程池
    server.thread_pool();

    //触发模式
    server.trig_mode();

    // 创建上传目录
    struct stat st;


    //监听
    server.eventListen();

    //运行
    server.eventLoop();

    return 0;
}