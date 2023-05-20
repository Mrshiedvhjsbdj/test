#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include "locker.h"
#include "threadpool.h"
#include <signal.h>
#include "http_conn.h"

#define MAX_FD 65535 //最大的文件描述符个数，并发客户端数量
#define MAX_EVENT_NUMBER 10000 //最大的监听事件数量

//注册信号捕捉
void addsig(int sig, void (handler)(int))
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    //bzero(&sa, sizeof(sa));
    sa.sa_handler = handler;
    //sa.sa_flags = 0;  //处理函数handler已作为参数传递，不需要指定handler了
    //设置临时阻塞信号集，全部阻塞？
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);
    
}

//添加文件描述符到epoll中
extern void addfd(int epollfd, int fd, bool one_shot);
//从epoll中删除文件描述符
extern void removefd(int epollfd, int fd);
//修改文件描述符
extern void modfd(int epollfd, int fd, int ev);


int main(int argc, char* argv[])
{
    if (argc <= 1) {
        printf("按照如下格式运行：%s port_number\n", basename(argv[0]));
        exit(-1);
    }

    //获取端口号
    int port = atoi(argv[1]);

    //网络通信时，一端断开连接，另一端继续写数据，会产生SIGPIPE信号
    //对SIGPIPE信号进行处理---默认终止进程；这里对SIGPIPE信号进行忽略
    addsig(SIGPIPE, SIG_IGN);

    //创建线程池（初始化线程池）
    threadpool<http_conn> * pool = NULL;
    //异常处理
    try {
        pool = new threadpool<http_conn>;
    } catch(...) {
        exit(-1);
    }
    
    //创建一个数组用于保存所有的客户端信息
    http_conn* users = new http_conn[MAX_FD];

    //创建监听套接字
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);

    //设置端口复用
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    //绑定
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    bind(listenfd, (struct sockaddr *)&address, sizeof(address));

    //监听
    listen(listenfd, 5);

    //创建epoll对象，事件数组(传出参数)，检测到事件产生，添加到事件数组中    
    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5);

    //将监听的文件描述符添加到epoll对象中
    addfd(epollfd, listenfd, false);
    http_conn::m_epollfd = epollfd;

    while (true) {
        int ret = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if ((ret < 0) && (errno != EINTR)) { //中断之后，epoll_wait不阻塞，返回值为-1，错误号为EINTR 
            printf("epoll failure\n");
            break;
        }

        //循环遍历事件数组
        for (int i = 0; i < ret; i++) {
            int sockfd = events[i].data.fd;
            if (sockfd == listenfd) {
                //有客户端连接进来
                struct sockaddr_in clientaddr;
                socklen_t len = sizeof(clientaddr);
                int connfd = accept(listenfd, (struct sockaddr *)&clientaddr, &len);       

                if (connfd < 0) {
                    printf("errno is:%d\n", errno);
                    continue;
                }     

                if (http_conn::m_user_count >= MAX_FD) {
                    //目前连接数满了
                    //给客户端写一个信息：服务器内部正忙。
                    close(connfd);
                    continue;
                }

                //将新的客户的数据初始化，放到数组中；将connfd添加到epoll实例中并监听
                users[connfd].init(connfd, clientaddr);
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                //对方异常断开或者错误等事件
                users[sockfd].close_conn();
            }
            //以下监测的EPOLLIN、EPOLLOUT事件均为connfd产生即else if (sockfd == connfd)
            else if (events[i].events & EPOLLIN) {
                if (users[sockfd].read()) { //一次性把所有数据都读完                    
                    pool->append(users + sockfd);
                }
                else { //读失败(或读完数据)，关闭连接
                    users[sockfd].close_conn();
                }
            }
            else if (events[i].events & EPOLLOUT) {
                if (!users[sockfd].write()) { //一次性写完所有数据
                    users[sockfd].close_conn(); //写失败时执行该代码
                }
            }
        }
    }

    close(epollfd);
    close(listenfd);
    delete [] users;
    delete pool;
    
    return 0;
}
