#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <pthread.h>
#include "lst_timer.h"

#define FD_LIMIT 65535
#define MAX_EVENT_NUMBER 1024
#define TIMESLOT 5

static int pipefd[2];
static sort_timer_lst timer_lst;
static int epollfd = 0;

//设置文件描述符非阻塞
void setnonblocking(int fd) {
    int flag = fcntl(fd, F_GETFL);
    flag |= O_NONBLOCK;
    fcntl(fd, F_SETFL, flag);
}

//添加文件描述符到epoll中
void addfd(int epollfd, int fd)
{
    epoll_event event;
    event.events = EPOLLIN | EPOLLET; //默认水平触发模式 (LT模式),这里使用边缘触发模式
    event.data.fd = fd;

    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    //如果使用ET模式，需要一次性读取全部数据；设置文件描述符非阻塞
    setnonblocking(fd);
}

//信号处理函数
void sig_handler(int sig) {
    int save_errno = errno;
    int msg = sig;  // 5,6,7,8.....
    send(pipefd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}

//注册信号捕捉
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

int main(int argc, char* argv[])
{
    if (argc <= 1) {
        printf("按照如下格式运行：%s port_number\n", basename(argv[0]));
        exit(-1);
    }

    //获取端口号
    int port = atoi(argv[1]);

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
    int ret = bind(listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret != -1);

    //监听
    ret = listen(listenfd, 5);
    assert(ret != -1);

    //创建epoll对象，事件数组(传出参数)，检测到事件产生，添加到事件数组中    
    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5);
    assert(epollfd != -1);

    //将监听的文件描述符添加到epoll对象中
    addfd(epollfd, listenfd);

    //创建管道
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    setnonblocking(pipefd[1]);
    addfd(epollfd, pipefd[0]);

    addsig(SIGALRM);
    addsig(SIGTERM);  // SIGTERM 发送本程序终止信号
    bool stop_server = false;

    client_data* users = new client_data[FD_LIMIT];
    bool timeout = false;
    alarm(TIMESLOT);  //定时，5秒后产生SIGALARM信号

    while (!stop_server) {
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if ((number < 0) && (errno != EINTR)) {
            printf("epoll failure\n");
            break;
        }

        for (int i = 0; i < number; i++) {
            int sockfd = events[i].data.fd;
            if (sockfd == listenfd) {
                struct  sockaddr_in clientaddr;
                socklen_t len = sizeof(clientaddr);
                int connfd = accept(listenfd, (sockaddr *)&clientaddr, &len);
                addfd(epollfd, connfd);
                users[connfd].address = clientaddr;
                users[connfd].sockfd = connfd;

                //创建定时器，设置其回调函数与超时时间，然后绑定定时器与用户数据，最后将定时器添加到链表timer_lst中
                util_timer* timer = new util_timer;
                timer->user_data = &users[connfd];
                timer->cb_func = cb_func;
                time_t cur = time(NULL);
                //timer->expire = cur + 9;
                timer->expire = cur + 3 * TIMESLOT; //超时时间为当前时间加15秒(绝对时间)
                users[connfd].timer = timer;
                timer_lst.add_timer(timer);                            
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
            else if (events[i].events & EPOLLIN) {
                memset(users[sockfd].buf, '\0', BUFSIZE);
                ret = recv(sockfd, users[sockfd].buf, BUFSIZE - 1, 0);
                printf("get %d bytes of client data %s from %d\n", ret, users[sockfd].buf, sockfd);
                util_timer* timer = users[sockfd].timer;
                if (ret < 0) {
                    //如果发送读错误，则关闭连接，并移除其对应的定时器
                    if (errno != EAGAIN) {
                        cb_func(&users[sockfd]);
                        if (timer) {
                            timer_lst.del_timer(timer);
                        }
                    }                    
                }
                else if (ret == 0) {
                    //如果对方已经关闭连接，则我们也关闭连接，并移除对应的定时器
                    cb_func(&users[sockfd]);
                    if (timer) {
                        timer_lst.del_timer(timer);
                    }
                }
                else {
                    //如果某个客户端上有数据可读，则我们要调整该连接对应的定时器，以延迟该连接被关闭的时间
                    if (timer) {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        printf("adjust timer once\n");
                        timer_lst.adjust_timer(timer);
                    }
                }
            }            
        }

        //最后处理定时事件，因为I/O事件有更高的优先级。当然，这样做将导致定时任务不能精准的按照预定的时间执行
        if (timeout) {
            timer_handler();
            timeout = false;
        }
    }

    close(listenfd);
    close(pipefd[1]);
    close(pipefd[0]);
    delete [] users;

    return 0;
}