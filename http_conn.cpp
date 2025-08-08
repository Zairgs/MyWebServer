#include "http_conn.h"
#include <dirent.h>
#include <mysql/mysql.h>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <openssl/sha.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>

#include <openssl/bio.h>
#include <openssl/evp.h>

#include <jsoncpp/json/json.h>
#include <algorithm>

// 定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

locker m_lock;
map<string, string> users;
std::map<std::string, std::set<http_conn *>> g_webrtc_rooms;
void http_conn::initmysql_result(connection_pool *connPool)
{
    // 先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    // 在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    // 从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    // 返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    // 返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    // 从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}
// 对文件描述符设置非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

// 将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

// 从内核时间表删除描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

// 将事件重置为EPOLLONESHOT
void modfd(int epollfd, int fd, int ev, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

// 关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        // 从房间中移除 - 只做一次
        if (!m_room.empty())
        {
            webrtc_lock.lock();
            auto room_it = g_webrtc_rooms.find(m_room);
            if (room_it != g_webrtc_rooms.end())
            {
                size_t count = room_it->second.erase(this);
                LOG_INFO("Removed %zu connections from room %s", count, m_room.c_str());

                if (room_it->second.empty())
                {
                    g_webrtc_rooms.erase(room_it);
                    LOG_INFO("Room %s emptied", m_room.c_str());
                }
            }
            webrtc_lock.unlock();
            m_room.clear(); // 清除房间信息
        }

        // 关闭上传文件
        if (m_upload_in_progress && m_upload_fd != -1)
        {
            close(m_upload_fd);
            m_upload_fd = -1;
        }

        // 关闭socket
        printf("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

// 初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode,
                     int close_log, string user, string passwd, string sqlname)
{
    m_sockfd = sockfd;
    m_address = addr;

    addfd(m_epollfd, sockfd, true, TRIGMode); // use incoming TRIGMode (m_TRIGMode not set yet)
    m_user_count++;

    // 当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root = root;
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();
}

// 初始化新接受的连接
// check_state默认为分析请求行状态
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    m_state = 0;
    timer_flag = 0;
    improv = 0;
    m_boundary.clear();

    m_headers.clear();
    // 初始化上传状态
    m_upload_in_progress = false;
    m_upload_fd = -1;
    m_upload_received = 0;
    m_upload_filename.clear();
    m_content_start_offset = 0;
    m_boundary_size = 0;
    m_file_content_ptr = nullptr;
    m_file_content_len = 0;

    m_is_upload = false;

    m_upgrade_to_websocket = false;
    m_isWebSocket = false;

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

// 从状态机，用于分析出一行内容
// 返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r')
        {
            if ((m_checked_idx + 1) == m_read_idx)
                return LINE_OPEN;
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if (temp == '\n')
        {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

// 循环读取客户数据，直到无数据可读或对方关闭连接
// 非阻塞ET工作模式下，需要一次性将数据读完
bool http_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read = 0;

    // LT读取数据
    if (0 == m_TRIGMode)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;

        if (bytes_read <= 0)
        {
            return false;
        }

        return true;
    }
    // ET读数据
    else
    {
        while (true)
        {
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                return false;
            }
            else if (bytes_read == 0)
            {
                return false;
            }
            m_read_idx += bytes_read;
        }
        return true;
    }
}

// 解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    m_url = strpbrk(text, " \t");
    if (!m_url)
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';
    char *method = text;
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else
        return BAD_REQUEST;
    m_url += strspn(m_url, " \t");
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }

    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    // 当url为/时，显示判断界面
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}
// 解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    // 先插入 map
    if (char *colon = strchr(text, ':'))
    {
        std::string key(text, colon - text);
    std::string value(colon + 1);

    // 去前后空白 —— 注意尾部要 +1
    key.erase(0, key.find_first_not_of(" \t"));
    size_t kpos = key.find_last_not_of(" \t");
    if (kpos != std::string::npos) key.erase(kpos + 1);

    value.erase(0, value.find_first_not_of(" \t"));
    size_t vpos = value.find_last_not_of(" \t\r\n");
    if (vpos != std::string::npos) value.erase(vpos + 1);

    m_headers[key] = value;
    }
    if (text[0] == '\0')
    {
        if (m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    else if (strncasecmp(text, "Upgrade:", 8) == 0)
    {
        text += 8;
        text += strspn(text, " \t");
        if (strcasecmp(text, "websocket") == 0)
            m_upgrade_to_websocket = true; // 新增成员变量
    }
    else if (strncasecmp(text, "Content-Type:", 13) == 0)
    {
        text += 13;
        text += strspn(text, " \t");

        // 检测是否是文件上传
        if (strstr(text, "multipart/form-data"))
        {
            char *boundary_start = strstr(text, "boundary=");
            if (boundary_start)
            {
                boundary_start += 9; // 跳过"boundary="
                // 处理带引号的边界
                if (*boundary_start == '"')
                {
                    boundary_start++;
                    char *boundary_end = strchr(boundary_start, '"');
                    if (boundary_end)
                    {
                        *boundary_end = '\0';
                        m_boundary = boundary_start;
                    }
                }
                else
                {
                    // 处理不带引号的边界
                    char *boundary_end = strpbrk(boundary_start, " ;\r\n");
                    if (boundary_end)
                    {
                        char save = *boundary_end;
                        *boundary_end = '\0';
                        m_boundary = boundary_start;
                        *boundary_end = save;
                    }
                }
                m_boundary_size = m_boundary.size() + 6;

                // 标记为上传请求
                m_is_upload = true;
            }
        }
    }
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;
        }
    }
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else
    {
        LOG_INFO("oop!unknow header: %s", text);
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    if (m_is_upload)
    { // 首次进入内容处理
        if (!m_upload_in_progress)
        {
            // 定位内容起始位置（跳过头部）
            char *content_start = strstr(text, "\r\n\r\n");
            if (!content_start)
                return BAD_REQUEST;

            m_content_start_offset = content_start + 4 - m_read_buf;
            m_file_content_ptr = content_start + 4;

            // 创建上传目录
            char upload_dir[FILENAME_LEN];
            snprintf(upload_dir, FILENAME_LEN, "%s/upload", doc_root);
            if (mkdir(upload_dir, 0777) == -1 && errno != EEXIST)
            {
                LOG_ERROR("Create upload dir failed: %s", upload_dir);
                return INTERNAL_ERROR;
            }

            // 解析文件名
            char *filename_start = strstr(text, "filename=\"");
            if (filename_start)
            {
                filename_start += 10;
                char *filename_end = strchr(filename_start, '"');
                if (filename_end)
                {
                    m_upload_filename.assign(filename_start, filename_end - filename_start);

                    // 文件名安全过滤
                    for (char &c : m_upload_filename)
                    {
                        if (strchr("/\\:*?\"<>|", c))
                            c = '_';
                    }
                }
            }

            // 默认文件名
            if (m_upload_filename.empty())
            {
                time_t now = time(nullptr);
                char default_name[256];
                snprintf(default_name, sizeof(default_name), "upload_%ld.jpg", now);
                m_upload_filename = default_name;
            }

            // 创建文件
            char save_path[FILENAME_LEN];
            snprintf(save_path, FILENAME_LEN, "%s/%s", upload_dir, m_upload_filename.c_str());

            m_upload_fd = open(save_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (m_upload_fd < 0)
            {
                LOG_ERROR("Open file failed: %s, error: %s", save_path, strerror(errno));
                return INTERNAL_ERROR;
            }

            LOG_INFO("Start upload: %s", save_path);
            m_upload_in_progress = true;
        }

        // 计算当前数据块的文件内容
        size_t data_len = m_read_idx - m_content_start_offset;
        char *data_start = m_read_buf + m_content_start_offset;

        // 查找结束边界
        std::string end_boundary = "--" + m_boundary + "--";
        char *boundary_pos = (char *)memmem(data_start, data_len, end_boundary.c_str(), end_boundary.size());

        if (boundary_pos)
        {
            // 找到结束边界 - 写入最后一块数据
            size_t file_data_len = boundary_pos - data_start - 2; // 减去前面的\r\n
            ssize_t written = ::write(m_upload_fd, data_start, file_data_len);
            if (written < 0 || (size_t)written != file_data_len)
            {
                LOG_ERROR("Final write failed: %s", strerror(errno));
                close(m_upload_fd);
                return INTERNAL_ERROR;
            }

            // 完成上传
            close(m_upload_fd);
            LOG_INFO("Upload complete: %s, size: %zu",
                     m_upload_filename.c_str(), m_upload_received + written);
            return UPLOAD_SUCCESS;
        }
        else
        {
            // 没有找到边界 - 写入整个数据块（保留边界空间）
            size_t safe_write_size = data_len - m_boundary_size;
            ssize_t written = ::write(m_upload_fd, data_start, safe_write_size);
            if (written < 0)
            {
                LOG_ERROR("Write failed: %s", strerror(errno));
                close(m_upload_fd);
                return INTERNAL_ERROR;
            }

            m_upload_received += written;

            // 保留最后部分数据（可能包含部分边界）
            size_t retain_size = data_len - safe_write_size;
            if (retain_size > 0)
            {
                memmove(m_read_buf, data_start + safe_write_size, retain_size);
                m_read_idx = retain_size;
                m_checked_idx = 0;
                m_content_start_offset = 0; // 新数据从缓冲区开始
            }
            else
            {
                m_read_idx = 0;
                m_checked_idx = 0;
            }

            return NO_REQUEST;
        }
    }
    // 普通表单处理（登录/注册）
    else
    {
        // 原始的表单数据处理逻辑
        if (m_read_idx >= (m_content_length + m_checked_idx))
        {
            text[m_content_length] = '\0';
            m_string = text;
            return GET_REQUEST;
        }
        return NO_REQUEST;
    }
}
http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line();
        m_start_line = m_checked_idx;
        // 跳过上传过程中的内容行解析
        // 跳过上传内容行的日志（避免二进制数据污染日志）
        if (m_check_state != CHECK_STATE_CONTENT || !m_is_upload)
        {
            LOG_INFO("%s", text);
        }
        if (!(m_upload_in_progress && m_check_state == CHECK_STATE_CONTENT))
        {
            LOG_INFO("%s", text);
        }
        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:
        {
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
        case CHECK_STATE_HEADER:
        {
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            else if (ret == GET_REQUEST)
            {
                return do_request();
            }
            break;
        }
        case CHECK_STATE_CONTENT:
        {
            ret = parse_content(text);
            if (ret == GET_REQUEST)
            {
                return do_request();
            }
            else if (ret == UPLOAD_SUCCESS)
            {
                return ret;
            }
            line_status = LINE_OPEN;
            break;
        }
        default:
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}
http_conn::HTTP_CODE http_conn::do_request()
{
    LOG_INFO("Processing request: URL=%s, Method=%d, Upload=%d",
             m_url, m_method, m_is_upload);

    // 1. 处理文件上传请求 (POST /upload)
    if (strcmp(m_url, "/upload") == 0 && m_method == POST && m_is_upload)
    {
        LOG_INFO("Handling file upload request");
        return handle_file_upload();
    }
    //  处理WebRTC信令请求 (POST /webrtc/<room_id>)
    if (strncmp(m_url, "/webrtc/", 8) == 0)
    {
        LOG_INFO("Handling WebRTC signaling request");
        return handle_webrtc_signaling();
    }
    // 处理WebSocket连接请求 (GET /webrtc)
    if (strcmp(m_url, "/webrtc") == 0 && m_method == GET)
    {
        LOG_INFO("Handling WebSocket connection request");
        return WEBSOCKET_UPGRADE;
    }
    // 处理视频通话页面请求 (GET /video_call.html)
    if (strcmp(m_url, "/video_call.html") == 0)
    {
        strcpy(m_real_file, doc_root);
        strcat(m_real_file, "/video_call.html");

        if (stat(m_real_file, &m_file_stat) < 0)
            return NO_RESOURCE;

        if (!(m_file_stat.st_mode & S_IROTH))
            return FORBIDDEN_REQUEST;

        if (S_ISDIR(m_file_stat.st_mode))
            return BAD_REQUEST;

        int fd = open(m_real_file, O_RDONLY);
        m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        return FILE_REQUEST;
    }
    // 2. 处理文件访问请求 (GET /upload/filename)
    if (strncmp(m_url, "/upload/", 8) == 0)
    {
        LOG_INFO("Handling file access request: %s", m_url);
        strcpy(m_real_file, doc_root);
        int len = strlen(doc_root);
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

        if (stat(m_real_file, &m_file_stat) < 0)
            return NO_RESOURCE;

        if (!(m_file_stat.st_mode & S_IROTH))
            return FORBIDDEN_REQUEST;

        if (S_ISDIR(m_file_stat.st_mode))
            return BAD_REQUEST;

        int fd = open(m_real_file, O_RDONLY);
        m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        return FILE_REQUEST;
    }

    // 3. 处理文件列表请求 (GET /upload)
    if (strcmp(m_url, "/upload") == 0 && m_method == GET)
    {
        LOG_INFO("Handling upload directory listing");
        char upload_dir[FILENAME_LEN];
        snprintf(upload_dir, FILENAME_LEN, "%s/upload", doc_root);

        DIR *dir = opendir(upload_dir);
        if (!dir)
        {
            return NO_RESOURCE;
        }

        std::string file_list = "<html><head><title>Upload Directory</title></head><body>";
        file_list += "<h1>Uploaded Files</h1><ul>";

        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL)
        {
            if (entry->d_name[0] != '.')
            {
                file_list += "<li><a href=\"/upload/";
                file_list += entry->d_name;
                file_list += "\">";
                file_list += entry->d_name;
                file_list += "</a></li>";
            }
        }
        closedir(dir);

        file_list += "</ul></body></html>";

        strncpy(m_write_buf, file_list.c_str(), WRITE_BUFFER_SIZE - 1);
        m_write_idx = file_list.length();

        add_status_line(200, ok_200_title);
        add_headers(m_write_idx);
        return FILE_REQUEST;
    }

    // 4. 处理登录和注册请求 (POST with URL ending in /2 or /3)
    const char *p = strrchr(m_url, '/');
    if (cgi == 1 && p != nullptr && (*(p + 1) == '2' || *(p + 1) == '3'))
    {
        LOG_INFO("Handling login/register request: %s", m_url);

        // 提取用户名和密码
        char name[100] = {0};
        char password[100] = {0};

        // 解析格式: user=username&passwd=password
        char *user_start = strstr(m_string, "user=");
        char *passwd_start = strstr(m_string, "passwd=");

        if (user_start && passwd_start)
        {
            // 解析用户名
            user_start += 5; // 跳过 "user="
            char *user_end = strchr(user_start, '&');
            if (user_end)
            {
                size_t user_len = user_end - user_start;
                strncpy(name, user_start, std::min(user_len, sizeof(name) - 1));
            }

            // 解析密码
            passwd_start += 7; // 跳过 "passwd="
            char *passwd_end = strchr(passwd_start, '\0');
            if (passwd_end)
            {
                size_t passwd_len = passwd_end - passwd_start;
                strncpy(password, passwd_start, std::min(passwd_len, sizeof(password) - 1));
            }
        }

        LOG_INFO("Parsed credentials: user=%s, passwd=%s", name, password);

        // 注册请求
        if (*(p + 1) == '3')
        {
            LOG_INFO("Processing registration for user: %s", name);
            char sql_insert[200];
            snprintf(sql_insert, sizeof(sql_insert),
                     "INSERT INTO user(username, passwd) VALUES('%s', '%s')",
                     name, password);

            if (users.find(name) == users.end())
            {
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                if (res == 0)
                {
                    users[name] = password;
                    strcpy(m_url, "/log.html");
                }
                else
                {
                    LOG_ERROR("Registration failed: %s", mysql_error(mysql));
                    strcpy(m_url, "/registerError.html");
                }
                m_lock.unlock();
            }
            else
            {
                LOG_WARN("User already exists: %s", name);
                strcpy(m_url, "/registerError.html");
            }
        }
        // 登录请求
        else if (*(p + 1) == '2')
        {
            LOG_INFO("Processing login for user: %s", name);
            if (users.find(name) != users.end() && users[name] == password)
            {
                LOG_INFO("Login successful: %s", name);
                strcpy(m_url, "/welcome.html");
            }
            else
            {
                LOG_WARN("Login failed: %s", name);
                strcpy(m_url, "/logError.html");
            }
        }
    }

    // 5. 处理其他静态文件请求
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);

    // 处理注册页面
    if (p != nullptr && *(p + 1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    // 处理登录页面
    else if (p != nullptr && *(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    else if (p != nullptr && *(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    // 处理图片页面
    else if (p != nullptr && *(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    // 处理视频页面
    else if (p != nullptr && *(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    // 处理特别推荐页面
    else if (p != nullptr && *(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    // 处理其他静态页面
    else
    {
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    }

    // 6. 返回请求的文件
    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;

    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;

    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;

    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}
void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}
bool http_conn::write()
{
    int temp = 0;

    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        init();
        return true;
    }

    while (1)
    {
        temp = writev(m_sockfd, m_iv, m_iv_count);

        if (temp < 0)
        {
            if (errno == EAGAIN)
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                return true;
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        if (bytes_to_send <= 0)
        {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);

            if (m_linger)
            {
                init();
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}
bool http_conn::add_response(const char *format, ...)
{
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);

    LOG_INFO("request:%s", m_write_buf);

    return true;
}
bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
bool http_conn::add_headers(int content_len)
{
    return add_content_length(content_len) && add_linger() &&
           add_blank_line();
}
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}
bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}
bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}
bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
        // 在process_write函数中添加
    case UPLOAD_SUCCESS:
    {
        add_status_line(200, "Upload Success");
        add_headers(0);
        add_content("<html><body>File uploaded successfully!</body></html>");
        break;
    }
    case WEBSOCKET_UPGRADE:
    {
        // 这个状态实际上不应该被写入，因为握手已经在websocket_handshake()中完成
        // 但为了完整性，我们可以添加一个空响应
        add_status_line(200, ok_200_title);
        add_headers(0);
        break;
    }
    case INTERNAL_ERROR:
    {
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form))
            return false;
        break;
    }
    case BAD_REQUEST:
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    case FILE_REQUEST:
    {
        add_status_line(200, ok_200_title);
        if (m_file_stat.st_size != 0)
        {
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            bytes_to_send = m_write_idx + m_file_stat.st_size;
            return true;
        }
        else
        {
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
                return false;
        }
    }
    default:
        return false;
    }
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}
void http_conn::process()
{
    HTTP_CODE read_ret = process_read();
    
    // 1. 处理 WebSocket 升级请求
    if (read_ret == WEBSOCKET_UPGRADE)
    {
        LOG_INFO("Processing WebSocket upgrade request");
        
// 检查是否有 Upgrade/Connection 头部（大小写不敏感，允许复合值）
auto has = [&](const char* k, const char* needle){
    auto it = m_headers.find(k);
    if (it == m_headers.end()) return false;
    std::string v = it->second;
    std::transform(v.begin(), v.end(), v.begin(), ::tolower);
    std::string n = needle;
    std::transform(n.begin(), n.end(), n.begin(), ::tolower);
    return v.find(n) != std::string::npos;
};
if (has("Upgrade", "websocket") && has("Connection", "upgrade"))
{
    websocket_handshake();
}
else
{
    LOG_ERROR("Invalid WebSocket upgrade request");
    add_status_line(400, "Bad Request");
    add_headers(0);
}
        return;
    }
    
    // 2. 如果已经是 WebSocket 连接，处理消息
    if (m_isWebSocket)
    {
        LOG_INFO("Processing WebSocket messages");
        
        bool has_messages = true;
        while (has_messages)
        {
            string msg = recvWebSocketMessage();
            if (!msg.empty())
            {
                LOG_INFO("WebSocket received message: %s", msg.c_str());
                handle_websocket_message(msg);
            }
            else
            {
                has_messages = false;
            }
        }
        
        // 重新注册事件
        modfd(m_epollfd, m_sockfd, EPOLLIN | EPOLLET | EPOLLONESHOT | EPOLLRDHUP, m_TRIGMode);
        improv = 1; // 标记有活动，刷新定时器
        return;
    }
    
    // 3. 处理普通 HTTP 请求
    LOG_INFO("process_read return: %d", read_ret);
    if (read_ret == NO_REQUEST)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return;
    }
    
    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
}
http_conn::HTTP_CODE http_conn::handle_file_upload()
{
    LOG_INFO("Handling file upload. Boundary: %s", m_boundary.c_str());

    // 1. 统一使用 upload 目录
    char upload_dir[FILENAME_LEN];
    snprintf(upload_dir, FILENAME_LEN, "%s/upload", doc_root);

    // 创建目录（如果不存在）
    struct stat st;
    if (stat(upload_dir, &st) == -1)
    {
        if (mkdir(upload_dir, 0777) == -1)
        {
            LOG_ERROR("Failed to create upload directory: %s, errno: %d", upload_dir, errno);
            return INTERNAL_ERROR;
        }
    }
    else if (!S_ISDIR(st.st_mode))
    {
        LOG_ERROR("Upload path exists but is not a directory: %s", upload_dir);
        return INTERNAL_ERROR;
    }

    // 2. 只处理body部分
    char *body_buf = m_read_buf + m_checked_idx;
    int body_len = m_read_idx - m_checked_idx;

    // 3. 定位文件内容起始位置（寻找 Content-Type 之后的空行）
    char *content_start = strstr(body_buf, "\r\n\r\n");
    if (!content_start)
    {
        LOG_ERROR("Failed to find content start (\\r\\n\\r\\n)");
        return BAD_REQUEST;
    }
    content_start += 4; // 跳过 \r\n\r\n

    // 4. 查找结束边界（使用完整的结束边界标记）
    std::string end_boundary = "--" + m_boundary + "--";
    char *content_end = strstr(content_start, end_boundary.c_str());
    if (!content_end)
    {
        // 尝试查找普通边界（不带 -- 后缀）
        std::string normal_boundary = "--" + m_boundary;
        content_end = strstr(content_start, normal_boundary.c_str());
        if (!content_end)
        {
            LOG_ERROR("Boundary not found in content. Boundary: %s", m_boundary.c_str());
            return BAD_REQUEST;
        }
    }
    content_end -= 2; // 回退到 \r\n 之前

    if (content_end <= content_start)
    {
        LOG_ERROR("Invalid content end position");
        return BAD_REQUEST;
    }

    // 5. 从 Content-Disposition 头部解析文件名
    char filename[FILENAME_LEN] = {0};
    char *disposition_start = strstr(body_buf, "Content-Disposition:");
    if (disposition_start)
    {
        char *filename_start = strstr(disposition_start, "filename=\"");
        if (filename_start)
        {
            filename_start += 10; // 跳过 "filename=\""
            char *filename_end = strchr(filename_start, '\"');
            if (filename_end)
            {
                size_t filename_len = filename_end - filename_start;
                if (filename_len < FILENAME_LEN - 1)
                {
                    strncpy(filename, filename_start, filename_len);
                    filename[filename_len] = '\0';
                }
                else
                {
                    LOG_WARN("Filename too long, truncating");
                    strncpy(filename, filename_start, FILENAME_LEN - 1);
                    filename[FILENAME_LEN - 1] = '\0';
                }
            }
        }
    }
    if (strlen(filename) == 0)
    {
        time_t now = time(nullptr);
        snprintf(filename, FILENAME_LEN, "upload_%ld.txt", now);
    }

    // 6. 安全过滤文件名
    char safe_filename[FILENAME_LEN] = {0};
    for (size_t i = 0, j = 0; i < strlen(filename) && j < FILENAME_LEN - 1; i++)
    {
        char c = filename[i];
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            safe_filename[j++] = '_';
        else
            safe_filename[j++] = c;
    }
    safe_filename[FILENAME_LEN - 1] = '\0';

    // 7. 保存文件到统一目录
    char save_path[FILENAME_LEN];
    snprintf(save_path, FILENAME_LEN, "%s/%s", upload_dir, safe_filename);
    LOG_INFO("Saving uploaded file to: %s", save_path);

    int fd = open(save_path, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
    {
        LOG_ERROR("File open failed: %s, error: %s", save_path, strerror(errno));
        return INTERNAL_ERROR;
    }

    size_t file_size = content_end - content_start;
    if (ftruncate(fd, file_size) == -1)
    {
        LOG_ERROR("ftruncate failed: %s", strerror(errno));
        close(fd);
        remove(save_path);
        return INTERNAL_ERROR;
    }

    void *map = mmap(NULL, file_size, PROT_WRITE | PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED)
    {
        LOG_ERROR("mmap failed: %s", strerror(errno));
        close(fd);
        remove(save_path);
        return INTERNAL_ERROR;
    }

    memcpy(map, content_start, file_size);

    if (msync(map, file_size, MS_SYNC) == -1)
    {
        LOG_ERROR("msync failed: %s", strerror(errno));
        munmap(map, file_size);
        close(fd);
        remove(save_path);
        return INTERNAL_ERROR;
    }

    munmap(map, file_size);
    close(fd);

    LOG_INFO("File uploaded successfully: %s, size: %zu", save_path, file_size);
    return UPLOAD_SUCCESS;
}

// WebRTC
//  静态成员初始化
locker http_conn::webrtc_lock;
std::map<std::string, http_conn::WebRTCSession> http_conn::webrtc_sessions;
time_t http_conn::last_cleanup_time = time(nullptr);

// JSON 转义函数
char *http_conn::json_escape(const char *input)
{
    if (!input)
        return nullptr;

    char *output = json_buffer;
    int out_index = 0;
    int in_len = strlen(input);
    int max_len = sizeof(json_buffer) - 1; // 保留一个字节给结束符

    for (int i = 0; i < in_len && out_index < max_len; i++)
    {
        switch (input[i])
        {
        case '"':
            if (out_index + 2 < max_len)
            {
                output[out_index++] = '\\';
                output[out_index++] = '"';
            }
            break;
        case '\\':
            if (out_index + 2 < max_len)
            {
                output[out_index++] = '\\';
                output[out_index++] = '\\';
            }
            break;
        case '\b':
            if (out_index + 2 < max_len)
            {
                output[out_index++] = '\\';
                output[out_index++] = 'b';
            }
            break;
        case '\f':
            if (out_index + 2 < max_len)
            {
                output[out_index++] = '\\';
                output[out_index++] = 'f';
            }
            break;
        case '\n':
            if (out_index + 2 < max_len)
            {
                output[out_index++] = '\\';
                output[out_index++] = 'n';
            }
            break;
        case '\r':
            if (out_index + 2 < max_len)
            {
                output[out_index++] = '\\';
                output[out_index++] = 'r';
            }
            break;
        case '\t':
            if (out_index + 2 < max_len)
            {
                output[out_index++] = '\\';
                output[out_index++] = 't';
            }
            break;
        default:
            output[out_index++] = input[i];
        }
    }

    output[out_index] = '\0';
    return output;
}

// 构建 JSON 响应
void http_conn::build_json_response(const char *json)
{
    add_status_line(200, ok_200_title);
    add_response("Content-Type: application/json\r\n");
    add_content_length(strlen(json));
    add_linger();
    add_blank_line();
    add_content(json);
}

// 清理过期房间
void http_conn::cleanup_room(const char *room_id)
{
    if (!room_id)
        return;

    time_t now = time(nullptr);
    std::string room_str(room_id);

    webrtc_lock.lock();

    // 清理特定房间
    auto it = webrtc_sessions.find(room_str);
    if (it != webrtc_sessions.end() && (now - it->second.last_active) > 300)
    { // 5分钟
        webrtc_sessions.erase(it);
        LOG_INFO("Cleaned up expired room: %s", room_id);
    }

    // 每小时全局清理
    if ((now - last_cleanup_time) > 3600)
    {
        for (auto it = webrtc_sessions.begin(); it != webrtc_sessions.end();)
        {
            if ((now - it->second.last_active) > 300)
            {
                it = webrtc_sessions.erase(it);
            }
            else
            {
                ++it;
            }
        }
        last_cleanup_time = now;
        LOG_INFO("Performed global WebRTC session cleanup");
    }

    webrtc_lock.unlock();
}

// WebRTC 信令入口
http_conn::HTTP_CODE http_conn::handle_webrtc_signaling()
{
    // 提取房间ID
    char room_id[100] = {0};
    char *room_start = strstr(m_url, "/webrtc/");
    if (!room_start)
        return BAD_REQUEST;

    room_start += 8; // 跳过 "/webrtc/"
    char *room_end = strchr(room_start, '/');
    if (!room_end)
        return BAD_REQUEST;

    size_t room_len = room_end - room_start;
    strncpy(room_id, room_start, std::min(room_len, sizeof(room_id) - 1));

    // 清理过期房间
    cleanup_room(room_id);

    // 提取信令类型
    char signal_type[20] = {0};
    char *type_start = room_end + 1;
    char *type_end = strchr(type_start, '/');
    if (!type_end)
    {
        strncpy(signal_type, type_start, sizeof(signal_type) - 1);
    }
    else
    {
        size_t type_len = type_end - type_start;
        strncpy(signal_type, type_start, std::min(type_len, sizeof(signal_type) - 1));
    }

    LOG_INFO("WebRTC signaling: room=%s, type=%s", room_id, signal_type);

    // 处理不同类型的信令
    if (strcmp(signal_type, "offer") == 0)
    {
        return handle_webrtc_offer(room_id);
    }
    else if (strcmp(signal_type, "answer") == 0)
    {
        return handle_webrtc_answer(room_id);
    }
    else if (strcmp(signal_type, "candidate") == 0)
    {
        return handle_webrtc_candidate(room_id);
    }

    return BAD_REQUEST;
}

// 处理 OFFER
http_conn::HTTP_CODE http_conn::handle_webrtc_offer(const char *room_id)
{
    if (m_method == POST)
    {
        // 存储offer
        webrtc_lock.lock();
        WebRTCSession &session = webrtc_sessions[room_id];
        session.offer = m_string;
session.last_active = time(nullptr);
        webrtc_lock.unlock();

        build_json_response("{\"status\":\"offer received\"}");
        return FILE_REQUEST;
    }
    else if (m_method == GET)
    {
        // 获取offer
        webrtc_lock.lock();
        auto it = webrtc_sessions.find(room_id);
        if (it == webrtc_sessions.end() || it->second.offer.empty())
        {
            webrtc_lock.unlock();
            return NO_RESOURCE;
        }

        char *escaped_offer = json_escape(it->second.offer.c_str());
        snprintf(json_buffer, sizeof(json_buffer),
                 "{\"type\":\"offer\", \"sdp\":\"%s\"}", escaped_offer);

        // 清理已获取的offer
        it->second.offer.clear();
        it->second.last_active = time(nullptr);

        webrtc_lock.unlock();

        build_json_response(json_buffer);
        return FILE_REQUEST;
    }
    return BAD_REQUEST;
}

// 处理 ANSWER
http_conn::HTTP_CODE http_conn::handle_webrtc_answer(const char *room_id)
{
    if (m_method == POST)
    {
        // 存储answer
        webrtc_lock.lock();
        WebRTCSession &session = webrtc_sessions[room_id];
        session.answer = m_string;
session.last_active = time(nullptr);
        webrtc_lock.unlock();

        build_json_response("{\"status\":\"answer received\"}");
        return FILE_REQUEST;
    }
    else if (m_method == GET)
    {
        // 获取answer
        webrtc_lock.lock();
        auto it = webrtc_sessions.find(room_id);
        if (it == webrtc_sessions.end() || it->second.answer.empty())
        {
            webrtc_lock.unlock();
            return NO_RESOURCE;
        }

        char *escaped_answer = json_escape(it->second.answer.c_str());
        snprintf(json_buffer, sizeof(json_buffer),
                 "{\"type\":\"answer\", \"sdp\":\"%s\"}", escaped_answer);

        // 清理已获取的answer
        it->second.answer.clear();
        it->second.last_active = time(nullptr);

        webrtc_lock.unlock();

        build_json_response(json_buffer);
        return FILE_REQUEST;
    }
    return BAD_REQUEST;
}

// 处理 ICE 候选
http_conn::HTTP_CODE http_conn::handle_webrtc_candidate(const char *room_id)
{
    if (m_method == POST)
    {
        // 存储候选
        webrtc_lock.lock();
        WebRTCSession &session = webrtc_sessions[room_id];
        session.candidates.push_back(m_string);
        session.last_active = time(nullptr);
        webrtc_lock.unlock();

        build_json_response("{\"status\":\"candidate received\"}");
        return FILE_REQUEST;
    }
    else if (m_method == GET)
    {
        // 获取候选
        webrtc_lock.lock();
        auto it = webrtc_sessions.find(room_id);
        if (it == webrtc_sessions.end() || it->second.candidates.empty())
        {
            webrtc_lock.unlock();
            return NO_RESOURCE;
        }

        // 构建候选数组
        std::string json = "{\"candidates\":[";
        for (size_t i = 0; i < it->second.candidates.size(); ++i)
        {
            char *escaped = json_escape(it->second.candidates[i].c_str());
            json += "\"";
            json += escaped;
            json += "\"";
            if (i < it->second.candidates.size() - 1)
            {
                json += ",";
            }
        }
        json += "]}";

        // 清理已获取的候选
        it->second.candidates.clear();
        it->second.last_active = time(nullptr);

        webrtc_lock.unlock();

        build_json_response(json.c_str());
        return FILE_REQUEST;
    }
    return BAD_REQUEST;
}
void http_conn::websocket_handshake()
{
    // 确保有Sec-WebSocket-Key头部
    if (m_headers.find("Sec-WebSocket-Key") == m_headers.end())
    {
        LOG_ERROR("Missing Sec-WebSocket-Key header");
        return;
    }

    string client_key = m_headers["Sec-WebSocket-Key"];
    string accept_key = client_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

    unsigned char sha1_result[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char *>(accept_key.c_str()),
         accept_key.length(),
         sha1_result);

    // 使用Base64编码
    BIO *b64 = BIO_new(BIO_f_base64());
    BIO *mem = BIO_new(BIO_s_mem());
    BIO_push(b64, mem);

    BIO_write(b64, sha1_result, SHA_DIGEST_LENGTH);
    BIO_flush(b64);

    BUF_MEM *bptr;
    BIO_get_mem_ptr(b64, &bptr);

    string encoded_key(bptr->data, bptr->length - 1); // 去掉尾部的换行符
    BIO_free_all(b64);

    // 构造握手响应
    string response = "HTTP/1.1 101 Switching Protocols\r\n";
    response += "Upgrade: websocket\r\n";
    response += "Connection: Upgrade\r\n";
    response += "Sec-WebSocket-Accept: " + encoded_key + "\r\n\r\n";

    // 发送握手响应
    int bytes_sent = send(m_sockfd, response.c_str(), response.length(), 0);
    if (bytes_sent <= 0)
    {
        LOG_ERROR("WebSocket handshake send failed: %s", strerror(errno));
    }
    else
    {
        LOG_INFO("WebSocket handshake completed");
        m_isWebSocket = true;
        improv = 1; // 刚握手成功，刷新连接定时器
        
        // 重置读缓冲区（重要！）
        m_read_idx = 0;
        m_checked_idx = 0;
        m_start_line = 0;
        memset(m_read_buf, '\0', READ_BUFFER_SIZE);
        
        // 重新注册事件
        modfd(m_epollfd, m_sockfd, EPOLLIN | EPOLLET | EPOLLONESHOT | EPOLLRDHUP, m_TRIGMode);
    }
}
void http_conn::sendWebSocketMessage(const string &msg)
{
    vector<unsigned char> frame;
    frame.push_back(0x81); // 文本帧 opcode=1，FIN=1
    if (msg.size() < 126)
    {
        frame.push_back(msg.size());
    }
    else if (msg.size() <= 0xFFFF)
    {
        frame.push_back(126);
        frame.push_back((msg.size() >> 8) & 0xFF);
        frame.push_back(msg.size() & 0xFF);
    } // 更大情况略

    frame.insert(frame.end(), msg.begin(), msg.end());
    send(m_sockfd, frame.data(), frame.size(), 0);
}

// 发送 WebSocket Pong 控制帧（用于回应 Ping）
void http_conn::sendWebSocketPong(const string &payload)
{
    std::vector<unsigned char> frame;
    frame.push_back(0x8A); // FIN=1, opcode=0xA (PONG)
    size_t n = payload.size();
    if (n < 126) {
        frame.push_back((unsigned char)n);
    } else if (n <= 0xFFFF) {
        frame.push_back(126);
        frame.push_back((n >> 8) & 0xFF);
        frame.push_back(n & 0xFF);
    } else {
        // 简化实现：不支持超长 PONG
        frame.push_back(127);
        for (int i = 7; i >= 0; --i) frame.push_back((n >> (8*i)) & 0xFF);
    }
    frame.insert(frame.end(), payload.begin(), payload.end());
    send(m_sockfd, reinterpret_cast<const char *>(frame.data()), frame.size(), 0);
}


string http_conn::recvWebSocketMessage()
{
    // 确保缓冲区有足够数据
    if (m_read_idx < 2)
    {
        return "";
    }

    unsigned char *buf = reinterpret_cast<unsigned char *>(m_read_buf);
    int opcode = buf[0] & 0x0F;

    // 处理关闭帧
    if (opcode == 0x08)
    {
        LOG_INFO("Received WebSocket close frame");
        close_conn();
        return "";
    }

    // 文本帧(0x1)，Ping(0x9)，Pong(0xA)，关闭帧(0x8) 处理
    bool is_text = (opcode == 0x01);
    bool is_ping = (opcode == 0x09);
    bool is_pong = (opcode == 0x0A);
    if (!(is_text || is_ping || is_pong)) {
        // 暂不支持二进制/分片等，直接丢弃该帧（但要消费缓冲区）
        LOG_WARN("Unsupported WebSocket opcode: %d", opcode);
        // 继续往下走，解析长度并丢弃负载
    }

    int payload_len = buf[1] & 0x7F;
    int header_size = 2;

    if (payload_len == 126)
    {
        if (m_read_idx < 4)
            return "";
        payload_len = (buf[2] << 8) | buf[3];
        header_size = 4;
    }
    else if (payload_len == 127)
    {
        LOG_ERROR("Oversized WebSocket frame not supported");
        return "";
    }

    bool masked = (buf[1] & 0x80) != 0;
    if (masked)
    {
        header_size += 4; // 掩码长度
    }

    // 检查帧是否完整
    if (m_read_idx < header_size + payload_len)
    {
        return ""; // 帧不完整
    }

    // 提取有效载荷
    char *payload_start = m_read_buf + header_size;
    string payload(payload_start, payload_len);

    // 如果使用了掩码，解码数据
    if (masked)
    {
        char mask[4] = {
            buf[header_size - 4],
            buf[header_size - 3],
            buf[header_size - 2],
            buf[header_size - 1]};

        for (int i = 0; i < payload_len; i++)
        {
            payload[i] ^= mask[i % 4];
        }
    }


    // 如果是 Ping，立即回 Pong
    if (is_ping) {
        sendWebSocketPong(payload);
        // 不返回应用层消息，继续消费帧
    }
    // 对于 Pong，不返回应用层消息
    if (is_pong) {
        // 忽略
    }

    // 移动缓冲区剩余数据
    int total_len = header_size + payload_len;
    int remaining = m_read_idx - total_len;

    if (remaining > 0)
    {
        memmove(m_read_buf, m_read_buf + total_len, remaining);
    }

    m_read_idx = remaining;
    m_checked_idx = 0;
    m_start_line = 0;

    return payload;
}
// 核心：收到WebSocket消息，转发给同房间其他人
void http_conn::handle_websocket_message(const std::string &msg)
{
    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(msg, root))
    {
        LOG_ERROR("Failed to parse WebSocket message as JSON");
        return;
    }

    std::string type = root.get("type", "").asString();
    std::string room = root.get("room", "").asString();

    // Keepalive / Ping 文本消息
    if (type == "keepalive" || type == "ping") {
        Json::FastWriter fw;
        Json::Value pong;
        pong["type"] = "pong";
        sendWebSocketMessage(fw.write(pong));
        return;
    }

    // 加入房间逻辑
    if (type == "join" && !room.empty())
    {
        m_room = room;
        webrtc_lock.lock();
        g_webrtc_rooms[room].insert(this);
        webrtc_lock.unlock();
        LOG_INFO("Client joined room: %s", room.c_str());
        return;
    }

    // ICE候选处理
    if (type == "candidate")
    {
        Json::Value candidate = root["candidate"];
        if (!candidate.isNull())
        {
            // 提取候选信息
            std::string candidate_str = candidate.get("candidate", "").asString();
            std::string sdp_mid = candidate.get("sdpMid", "").asString();
            int sdp_mline_index = candidate.get("sdpMLineIndex", -1).asInt();

            if (!candidate_str.empty() && sdp_mline_index >= 0)
            {
                // 构造候选消息
                Json::Value candidate_msg;
                candidate_msg["type"] = "candidate";
                candidate_msg["candidate"] = candidate_str;
                candidate_msg["sdpMid"] = sdp_mid;
                candidate_msg["sdpMLineIndex"] = sdp_mline_index;

                Json::FastWriter writer;
                std::string candidate_json = writer.write(candidate_msg);

                // 转发给同房间其他客户端
                webrtc_lock.lock();
                auto room_it = g_webrtc_rooms.find(room);
                if (room_it != g_webrtc_rooms.end())
                {
                    for (auto *peer : room_it->second)
                    {
                        if (peer != this)
                        {
                            peer->sendWebSocketMessage(candidate_json);
                        }
                    }
                }
                webrtc_lock.unlock();
            }
        }
        return;
    }

    // 离开房间逻辑
    if (type == "leave" && !m_room.empty())
    {
        webrtc_lock.lock();
        auto it = g_webrtc_rooms.find(m_room);
        if (it != g_webrtc_rooms.end())
        {
            it->second.erase(this);
            if (it->second.empty())
            {
                g_webrtc_rooms.erase(it);
            }
        }
        webrtc_lock.unlock();
        LOG_INFO("Client left room: %s", m_room.c_str());
        m_room.clear();
        return;
    }

    // 转发WebRTC信令
    if (!m_room.empty())
    {
        webrtc_lock.lock();
        auto room_it = g_webrtc_rooms.find(m_room);
        if (room_it != g_webrtc_rooms.end())
        {
            for (auto *peer : room_it->second)
            {
                if (peer != this)
                {
                    peer->sendWebSocketMessage(msg);
                }
            }
        }
        webrtc_lock.unlock();
    }
}