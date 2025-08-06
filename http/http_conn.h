#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <map>

#include <vector>
#include <string>
#include <ctime>

#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"
#include "../timer/lst_timer.h"
#include "../log/log.h"

class http_conn
{
public:
    static const int FILENAME_LEN = 200;
    static const int READ_BUFFER_SIZE = 65536;
    static const int WRITE_BUFFER_SIZE = 1024;
    enum METHOD
    {
        GET = 0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATH
    };
    enum CHECK_STATE
    {
        CHECK_STATE_REQUESTLINE = 0,
        CHECK_STATE_HEADER,
        CHECK_STATE_CONTENT
    };
    enum HTTP_CODE
    {
        NO_REQUEST,
        GET_REQUEST,
        BAD_REQUEST,
        NO_RESOURCE,
        FORBIDDEN_REQUEST,
        FILE_REQUEST,
        INTERNAL_ERROR,
        CLOSED_CONNECTION,
        UPLOAD_SUCCESS,
        UPLOAD_FAILURE
    };
    enum LINE_STATUS
    {
        LINE_OK = 0,
        LINE_BAD,
        LINE_OPEN
    };

public:
    http_conn() {}
    ~http_conn() {}

public:
    void init(int sockfd, const sockaddr_in &addr, char *, int, int, string user, string passwd, string sqlname);
    void close_conn(bool real_close = true);
    void process();
    bool read_once();
    bool write();
    sockaddr_in *get_address()
    {
        return &m_address;
    }
    void initmysql_result(connection_pool *connPool);
    int timer_flag;
    int improv;

    // WebRTC 相关函数
    HTTP_CODE handle_webrtc_signaling();
    HTTP_CODE handle_webrtc_offer(const char* room_id);
    HTTP_CODE handle_webrtc_answer(const char* room_id);
    HTTP_CODE handle_webrtc_candidate(const char* room_id);
    void build_json_response(const char* json);
    char* json_escape(const char* input);
    void cleanup_room(const char* room_id);
    
    // WebRTC 信令数据结构
    struct WebRTCSession {
        char offer[4096];
        char answer[4096];
        std::vector<std::string> candidates;
        time_t last_active;
    };
    
    // 静态成员用于存储房间信息
    static locker webrtc_lock;
    static std::map<std::string, WebRTCSession> webrtc_sessions;
    static time_t last_cleanup_time;

private:
    void init();
    HTTP_CODE process_read();
    bool process_write(HTTP_CODE ret);
    HTTP_CODE parse_request_line(char *text);
    HTTP_CODE parse_headers(char *text);
    HTTP_CODE parse_content(char *text);
    HTTP_CODE do_request();
    HTTP_CODE handle_file_upload();
    
    char *get_line() { return m_read_buf + m_start_line; };
    LINE_STATUS parse_line();
    void unmap();
    bool add_response(const char *format, ...);
    bool add_content(const char *content);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

public:
    static int m_epollfd;
    static int m_user_count;
    MYSQL *mysql;
    int m_state; // 读为0, 写为1

private:
    int m_sockfd;
    sockaddr_in m_address;
    char m_read_buf[READ_BUFFER_SIZE];
    long m_read_idx;
    long m_checked_idx;
    int m_start_line;
    char m_write_buf[WRITE_BUFFER_SIZE];
    int m_write_idx;
    CHECK_STATE m_check_state;
    METHOD m_method;
    char m_real_file[FILENAME_LEN];
    char *m_url;
    char *m_version;
    char *m_host;
    long m_content_length;
    bool m_linger;
    char *m_file_address;
    struct stat m_file_stat;
    struct iovec m_iv[2];
    int m_iv_count;
    int cgi;        // 是否启用的POST
    char *m_string; // 存储请求头数据
    int bytes_to_send;
    int bytes_have_send;
    char *doc_root;
    std::string m_boundary;

     // 大文件上传状态跟踪
    bool m_upload_in_progress;
    int m_upload_fd;
    size_t m_upload_received;
    std::string m_upload_filename;
    size_t m_content_start_offset; // 内容起始偏移
    size_t m_boundary_size;        // 边界字符串长度
    
    // 新增缓冲区管理
    char* m_file_content_ptr;      // 文件内容指针
    size_t m_file_content_len;     // 文件内容长度

    // 添加上传标记
    bool m_is_upload;  // 标记当前请求是否为文件上传

    
    int m_TRIGMode;
    int m_close_log;

    char sql_user[100];
    char sql_passwd[100];
    char sql_name[100];
    bool m_upload_started = false; // 是否已进入文件内容写入阶段
    size_t m_upload_content_start = 0; // 内容起始偏移

    char json_buffer[4096]; // 用于构建JSON响应
};

#endif
