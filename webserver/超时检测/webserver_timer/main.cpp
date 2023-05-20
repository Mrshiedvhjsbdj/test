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
#include "lst_timer.h"

#define MAX_FD 65535 //最大的文件描述符个数，并发客户端数量
#define MAX_EVENT_NUMBER 10000 //最大的监听事件数量
#define TIMESLOT 5

static int pipefd[2];
static sort_timer_lst timer_lst;
static int epollfd = 0;

//信号处理函数
void sig_handler(int sig) {
    int save_errno = errno;
    int msg = sig;  // 5,6,7,8.....
    send(pipefd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}

//注册定时器信号捕捉
void addsig(int sig)
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    //bzero(&sa, sizeof(sa));
    sa.sa_handler = sig_handler;
    sa.sa_flags |= SA_RESTART;  //SA_RESTART 由此信号中断的系统调用自动重启动
    //设置临时阻塞信号集，全部阻塞？
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);
}

void timer_handler() {
    //定时处理任务，实际上就是调用tick()函数
    timer_lst.tick();
    //因为一次alarm调用只会引起一次SIGALARM信号，所有我们要重新定时，以不断触发SIGALARM信号。
    alarm(TIMESLOT);
}

//定时器回调函数，它删除非活动连接socket上的注册事件，并关闭之。
void cb_func(client_data* user_data) {
    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    close(user_data->sockfd);
    printf("close fd %d\n", user_data->sockfd);
}

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
extern void setnonblocking(int fd);



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

    //创建管道
    int ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    setnonblocking(pipefd[1]);
    addfd(epollfd, pipefd[0], false);

    //定时器信号捕捉
    addsig(SIGALRM);
    addsig(SIGTERM);
    bool stop_server = false;

    client_data* time_users = new client_data[MAX_FD];
    bool timeout = false;
    alarm(TIMESLOT);  //定时，5秒后产生SIGALARM信号

    while (!stop_server) {
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
                time_users[connfd].address = clientaddr;
                time_users[connfd].sockfd = connfd;       

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

                //创建定时器，设置其回调函数与超时时间，然后绑定定时器与用户数据，最后将定时器添加到链表timer_lst中
                util_timer* timer = new util_timer;
                timer->user_data = &time_users[connfd];
                timer->cb_func = cb_func;
                time_t cur = time(NULL);
                //timer->expire = cur + 9;
                timer->expire = cur + 3 * TIMESLOT; //超时时间为当前时间加15秒(绝对时间)
                time_users[connfd].timer = timer;
                timer_lst.add_timer(timer);

                //将新的客户的数据初始化，放到数组中；将connfd添加到epoll实例中并监听
                users[connfd].init(connfd, clientaddr);
            }
            else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN)) {
                //处理信号
                int sig;
                char signals[1024];
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if (ret == -1) {
                    continue;
                }
                else if (ret == 0) {
                    continue;
                }
                else {
                    for (int i = 0; i < ret; i++) {
                        switch (signals[i]) {
                            case SIGALRM:
                                //用timeout变量标记有定时任务需要处理，但不立即处理定时任务，这是因为定时任务的优先级不是很高，我们优先处理其他更重要的任务。
                                timeout = true;                                
                                break;
                            case SIGTERM:
                                stop_server = true;
                        }                                                                                                                      
                    }
                }
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                //对方异常断开或者错误等事件
                users[sockfd].close_conn();
            }
            //以下监测的EPOLLIN、EPOLLOUT事件均为connfd产生即else if (sockfd == connfd)
            else if (events[i].events & EPOLLIN) { 
                util_timer* timer = time_users[sockfd].timer;               
                if (users[sockfd].read()) { //一次性把所有数据都读完
                    //如果某个客户端上有数据可读，则我们要调整该连接对应的定时器，以延迟该连接被关闭的时间
                    if (timer) {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        printf("adjust timer once\n");
                        timer_lst.adjust_timer(timer);
                    }                    
                    pool->append(users + sockfd);
                }
                else { //读失败(或读完数据)，关闭连接,并移除对应的定时器
                    cb_func(&time_users[sockfd]);
                    if (timer) {
                        timer_lst.del_timer(timer);
                    }
                    users[sockfd].close_conn();
                }
            }
            else if (events[i].events & EPOLLOUT) {
                if (!users[sockfd].write()) { //一次性写完所有数据
                    users[sockfd].close_conn(); //写失败时执行该代码
                }
            }
        }

        //最后处理定时事件，因为I/O事件有更高的优先级。当然，这样做将导致定时任务不能精准的按照预定的时间执行
        if (timeout) {
            timer_handler();
            timeout = false;
        }
    }

    close(epollfd);
    close(listenfd);
    close(pipefd[1]);
    close(pipefd[0]);
    delete [] time_users;
    delete [] users;
    delete pool;
    
    return 0;
}
