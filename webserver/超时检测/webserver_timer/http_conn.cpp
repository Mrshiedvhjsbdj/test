#include "http_conn.h"

//初始化静态变量
int http_conn::m_epollfd = -1;
int http_conn::m_user_count = 0;

//定义HTTP响应的一些状态信息
const char * ok_200_title = "OK";
const char * error_400_title = "Bad Request";
const char * error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char * error_403_title = "Forbidden";
const char * error_403_form = "You do not have permission to get file from this server.\n";
const char * error_404_title = "Not Found";
const char * error_404_form = "Ther requseted file was not found on this server.\n";
const char * error_500_title = "Internal Error";
const char * error_500_form = "There was an unusual problem serving the requested file.\n";

// 网站的根目录
const char * doc_root = "/home/liu/linux/webserver/resources";

//设置文件描述符非阻塞
void setnonblocking(int fd) {
    int flag = fcntl(fd, F_GETFL);
    flag |= O_NONBLOCK;
    fcntl(fd, F_SETFL, flag);
}

//添加文件描述符到epoll中
void addfd(int epollfd, int fd, bool one_shot)
{
    epoll_event event;
    event.events = EPOLLIN | EPOLLRDHUP; //默认水平触发模式 (LT模式)、EPOLLRDHUP(挂起)：对方连接断开时会触发，可在底层处理，不再需要移交上层处理(recv返回值判断)
    event.data.fd = fd;

    if (one_shot) {
        event.events |= EPOLLONESHOT;
    }

    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    //如果使用ET模式，需要一次性读取全部数据；设置文件描述符非阻塞
    setnonblocking(fd);
}

//从epoll中删除文件描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

//修改文件描述符,重置socket上EPOLLONESHOT事件，以确保下一次可读时，EPOLLIN事件能被触发；下一次可写时，EPOLLOUT事件能被触发。
void modfd(int epollfd, int fd, int ev)
{
    epoll_event event;
    event.data.fd =fd; 
    event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

//初始化接收的连接  (！！！编写过程遇到的bug，将sockfd赋值给m_epollfd)
void http_conn::init(int sockfd, const sockaddr_in& addr)
{
    m_sockfd = sockfd; //初始化之后，可在http_conn类中通过fd进行读写操作
    m_address = addr;

    //设置端口复用
    int reuse = 1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    //添加到epoll对象中
    addfd(m_epollfd, m_sockfd, true);
    m_user_count++; //总用户数+1

    init();
}

//初始化连接的其余的信息
void http_conn::init()
{
    m_check_state = CHECK_STATE_REQUESTLINE;  //初始化状态为解析请求首行
    m_checked_idx = 0;
    m_start_line = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    m_method = GET;
    m_host = 0;
    m_url = 0;
    m_version = 0;
    m_linger = false;
    m_content_length = 0;

    bzero(m_read_buf, READ_BUF_SIZE);
    bzero(m_write_buf, WRITE_BUF_SIZE);
    bzero(m_real_file, FILENAME_LEN);
}

//关闭连接
void http_conn::close_conn()
{
    if (m_sockfd != -1) {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--; //关闭一个连接，客户总数量-1
    }
}

//循环读取客户数据，直到无数据可读或者对方关闭连接
bool http_conn::read()
{
    if (m_read_idx >= READ_BUF_SIZE) {
        return false;
    }

    //读取到的字节
    int bytes_read = 0;
    while (true) {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUF_SIZE - m_read_idx, 0);
        if (bytes_read == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                //没有数据，
                break;
            }
            return false;
        }
        else if (bytes_read == 0) {
            //对方关闭连接
            return false;
        }
        //else 读取到了
        m_read_idx += bytes_read;
    }
    //printf("读取到了数据：%s\n", m_read_buf);

    return true;
}

//主状态机，解析HTTP请求
http_conn::HTTP_CODE http_conn::process_read() 
{
    //初始化从状态机状态
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;

    char * text = 0; // char * text = NULL;

    while (((m_check_state == CHECK_STATE_CONTENT) && ((line_status = parse_line()) == LINE_OK))
     || (line_status = parse_line()) == LINE_OK) {
        //解析到了请求体，且请求体也是完整的数据，或者解析到了一行完整的数据

        //获取一行数据
        text = get_line();  //第一次从m_read_buf[0]开始获取(m_staat_line = 0)

        m_start_line = m_checked_idx;
        printf("got i http line: %s\n", text);

        switch(m_check_state) {
            case CHECK_STATE_REQUESTLINE:
            {
                ret = parse_request_line(text);
                if (ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                }
                break;
            }

            case CHECK_STATE_HEADER:
            {
                ret = parse_handers(text);
                if (ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                }
                else if (ret == GET_REQUEST) {
                    return do_request();
                }
                break;
            }

            case CHECK_STATE_CONTENT:
            {
                ret = parse_content(text);
                if (ret == GET_REQUEST) {//获取完整的请求
                    return do_request();
                }
                //请求 行数据不完整
                line_status = LINE_OPEN;
                break;
            }

            default:
            {
                return INTERNAL_ERROR;
            }
        }

    }   
    return NO_REQUEST;
}

//解析HTTP请求行数据，获得请求方法、目标URL、HTTP版本
http_conn::HTTP_CODE http_conn::parse_request_line(char * text) 
{
    //1.获取m_method
    // GET /index.html HTTP/1.1
    m_url = strpbrk(text, " \t");  //strpbrk返回值为检测到的第一个空格或'\t',这里为GET后的空格 (指针，指向空格)
    if (!m_url) {
        return BAD_REQUEST;
    }

    // GET\0/index.html HTTP/1.1
    *m_url++ = '\0';  //m_url指向 "/index.html HTTP/1.1"

    //GET\0
    char * method = text; //'\0'为字符串结束符，获取到的method为 GET
    if (strcasecmp(method, "GET") == 0) { //strcasecmp为忽略大小写比较
        m_method = GET;
    }
    else {
        return BAD_REQUEST;
    }

    //2.获取m_version
    // /index.html HTTP/1.1
    m_version = strpbrk(m_url, " \t"); //m_version 指向 " HTTP/1.1"
    if (!m_version) {
        return BAD_REQUEST;
    }

    // /index.html\0HTTP/1.1
    *m_version++ = '\0';  //m_version 指向 "HTTP/1.1"
    if (strcasecmp(m_version, "HTTP/1.1") != 0) {
        return BAD_REQUEST;
    }

    //3.获取m_url
    // http://192.168.117.128:10000/index.html
    if (strncasecmp(m_url, "HTTP://", 7) == 0) {
        m_url += 7; //m_url为 "192.168.117.128:10000/index.html"
        m_url = strchr(m_url, '/'); //m_url为 "/index.html"
    }

    if ((!m_url) || (m_url[0] != '/')) {
        return BAD_REQUEST;
    }

    m_check_state = CHECK_STATE_HEADER; //主状态机 检查请求行 变成 检查请求头

    return NO_REQUEST;
}

//解析HTTP请求头信息
http_conn::HTTP_CODE http_conn::parse_handers(char * text)
{
    //遇到空行，表示请求头解析完毕，主状态机 检查请求头 变成 检查请求体
    if (text[0] == '\0') {
        //如果HTTP请求有请求体，则还需要读取m_content_length字节的请求体
        if (m_content_length != 0) {
            m_check_state = CHECK_STATE_CONTENT; //主状态机变成检查请求体
            return NO_REQUEST;
        }

        //否则说明我们已经得到了一个完整的HTTP请求
        return GET_REQUEST;
    }
    else if (strncasecmp(text, "Connection:", 11) == 0) {  //"Connection: keep-alive"
        //处理Connection 头部字段， Connection: keep-alive -- 保持连接
        text += 11;  //" keep-alive"
        text += strspn(text, " \t");  //"keep-alive"
        if (strcasecmp(text, "keep-alive") == 0) {
            m_linger = true;  //保持连接
        }
    }
    else if (strncasecmp(text, "Content-Length:", 15) == 0) {  //"Content-Length: 1000"
        //处理Content-Length头部字段
        text += 15;  //" 1000"
        text += strspn(text, " \t"); //"1000"
        m_content_length = atol(text);
    }
    else if (strncasecmp(text, "Host:", 5) == 0) {  //"Host: 192.168.117.128:10000"
        //处理Host头部字段
        text += 5;  //" 192.168.117.128:10000"
        text += strspn(text, " \t");  //"192.168.117.128:10000"
        m_host = text;
    }
    else {
        printf("oop! unknow header %s\n", text);
    }

    return NO_REQUEST;
}

//我们没有真正解析HTTP请求体，只是判断它是否被完整地读入了
http_conn::HTTP_CODE http_conn::parse_content(char * text)
{
    if (m_read_idx >= (m_checked_idx + m_content_length)) {
        text[m_content_length] = '\0';
        return GET_REQUEST;
    }

    return NO_REQUEST;
}

//解析一行，判断依据\r\n
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;

    for ( ; m_checked_idx < m_read_idx; m_checked_idx++) {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r') {
            if ((m_checked_idx + 1) == m_read_idx) {
                return LINE_OPEN;
            }
            else if (m_read_buf[m_checked_idx + 1] == '\n') {
                m_read_buf[m_checked_idx++] = '\0';  //将'\r'置为'\0',m_checked_idx指向'\n'
                m_read_buf[m_checked_idx++] = '\0';  //将'\n'置为'\0',m_checked_idx指向下一行起始位置
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if (temp == '\n') {
            if ((m_checked_idx > 1) && (m_read_buf[m_checked_idx - 1] == '\r')) {
                m_read_buf[m_checked_idx - 1] = '\0';  //将'\r'置为'\0'
                m_read_buf[m_checked_idx++] = '\0';   //将'\n'置为'\0',m_checked_idx指向下一行起始位置
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

/*
    当得到一个完整、正确的HTTP请求时，我们就分析目标文件的属性，
    如果目标文件存在、对所有用户可读，且不是目录，则使用mmap将其映射到内存地址 m_file_address处，并告诉调用者获取文件成功
*/
http_conn::HTTP_CODE http_conn::do_request()
{
    // "/home/liu/linux/webserver/resources"
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    // "/home/liu/linux/webserver/resources/index.html"
    strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    //获取m_real_file文件的相关的状态信息，-1失败(不存在)，0成功(存在) m_file_stat 是传出参数
    if (stat(m_real_file, &m_file_stat) < 0) {
        return NO_RESOURCE;
    }

    //判断访问权限(读权限，没有则返回0)
    if (!(m_file_stat.st_mode & S_IROTH)) {
        return FORBIDDEN_REQUEST;
    }

    //判断是否是目录(是目录则返回 1)
    if (S_ISDIR(m_file_stat.st_mode)) {
        return BAD_REQUEST;
    }

    //以只读方式打开文件
    int fd = open(m_real_file, O_RDONLY);

    //创建内存映射
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    return FILE_REQUEST;
}

//对内存映射区执行munmap操作(释放内存映射)
void http_conn::unmap()
{
    if (m_file_address) {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}



//往写缓冲中写入待发送的数据
bool http_conn::add_response(const char * format, ...)
{
    if (m_write_idx >= WRITE_BUF_SIZE) {
        return false;
    }

    //va_* \ vsnprintf
    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUF_SIZE - 1 - m_write_idx, format, arg_list);
    if (len >= (WRITE_BUF_SIZE - 1 - m_write_idx)) {
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);

    return true;
}

//增加请求行
bool http_conn::add_status_line(int status, const char * title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

//增加请求头
bool http_conn::add_headers(int content_len)
{
    add_content_length(content_len);
    add_content_type();
    add_linger();
    add_blank_line();
    
    return true;
}

//增加请求体
bool http_conn::add_content(const char * content)
{
    return add_response("%s", content);
}

bool http_conn::add_content_type()
{
    return add_response("Content-Type: %s\r\n", "text/html");
}

bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length: %d\r\n", content_len);
}

bool http_conn::add_linger()
{
    return add_response("Connection: %s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}

//根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret) {
        case INTERNAL_ERROR:
        {
            add_status_line(500, error_500_title);
            add_headers(strlen(error_500_form));
            if (!add_content(error_500_form)) {
                return false;
            }
            break;
        }
        case BAD_REQUEST:
        {
            add_status_line(400, error_400_title);
            add_headers(strlen(error_400_form));
            if (!add_content(error_400_form)) {
                return false;
            }
            break;
        }
        case NO_RESOURCE:
        {
            add_status_line(404, error_404_title);
            add_headers(strlen(error_404_form));
            if (!add_content(error_404_form)) {
                return false;
            }
            break;
        }
        case FORBIDDEN_REQUEST:
        {
            add_status_line(403, error_403_title);
            add_headers(strlen(error_403_form));
            if (!add_content(error_403_form)) {
                return false;
            }
            break;
        }
        case FILE_REQUEST:
        {
            add_status_line(200, ok_200_title);
            add_headers(m_file_stat.st_size);

            //获取文件成功，将写缓冲区和文件的内容输出
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;

            return true;
        }
        default:
            return false;   
    }

    //出错时(获取文件失败)将写缓冲区的内容输出。
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;

    return true;
}

//写HTTP响应
bool http_conn::write()
{
    int temp = 0;
    int bytes_have_send = 0;  //已经发送的字节
    int bytes_to_send = m_write_idx;  //将要发送的字节,(m_write_idx)写缓冲区中待发送的字节数

    if (bytes_to_send = 0) {
        //将要发送的字节为0，这一次响应结束。重新监听客户端输入，并初始化。
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        init();
        return true;
    }

    while (1) {
        //汇聚写(gather output)---writev
        temp = writev(m_sockfd, m_iv, m_iv_count);
        if (temp <= -1) {
            //如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间，服务器无法立即收到同一客户的下一个请求，但可以保证连接的完整性。
            if (errno == EAGAIN) {
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            unmap();
            return false;
        }
        bytes_to_send -= temp;
        bytes_have_send += temp;
        if (bytes_to_send <= bytes_have_send) {
            //发送HTTP响应成功，根据HTTP请求中的Connection字段决定是否立即关闭连接
            unmap();
            if (m_linger) { //保持连接，重新监听客户端输入，并初始化
                init();
                modfd(m_epollfd, m_sockfd, EPOLLIN);
                return true;
            }
            else {
                modfd(m_epollfd, m_sockfd, EPOLLIN);
                return false;
            }
        }

    }
}

//由线程池中的工作线程调用，这是处理HTTP请求的入口函数
void http_conn::process()
{
    //解析HTTP请求
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST) { //请求报文不完整，修改m_sockfd,重新监听EPOLLIN，继续获取客户端数据
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }

    //生成响应
    bool write_ret = process_write(read_ret);
    if (!write_ret) {
        close_conn();
    }
    //修改文件描述符，下一次可写时，EPOLLOUT事件能被触发。
    modfd(m_epollfd, m_sockfd, EPOLLOUT);

}