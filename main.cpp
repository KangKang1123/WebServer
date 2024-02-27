#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>
#include <string.h>
#include "./lock/locker.h"
#include "./threadpool/threadpool.h"
#include "./http/http_conn.h"
#include "timer/lst_timer.h"

#define MAX_FD 65536           //最大文件描述符
#define MAX_EVENT_NUMBER 10000 //最大事件数
#define TIMESLOT 5             //最小超时单位

extern int addfd(int epollfd, int fd, bool one_shot);

extern int removefd(int epollfd, int fd);

extern int setnonblocking(int fd);

//设置定时器相关参数
static int pipefd[2];
static int epollfd = 0;
//创建定时器容器链表
static sort_timer_lst timer_lst;

//信号处理函数
void sig_handler(int sig) {
    //为保证函数的可重入性，保留原来的errno
    //可重入性表示中断后再次进入该函数，环境变量与之前相同，不会丢失数据
    int save_errno = errno;
    int msg = sig;
    //将信号值从管道写端写入，传输字符类型，而非整型
    send(pipefd[1], (char *) &msg, 1, 0);
    //将原来的errno赋值为当前的errno
    errno = save_errno;
}

//设置信号函数
void addsig(int sig, void(handler)(int), bool restart = true) {
    //创建sigaction结构体变量
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));

    //信号处理函数中仅仅发送信号值，不做对应逻辑处理
    sa.sa_handler = handler;
    if (restart)
        sa.sa_flags |= SA_RESTART;
    //将所有信号添加到信号集中
    sigfillset(&sa.sa_mask);
    //执行sigaction函数
    assert(sigaction(sig, &sa, NULL) != -1);
}

//定时处理任务，重新定时以不断触发SIGALRM信号
void timer_handler() {
    timer_lst.tick();
    alarm(TIMESLOT);
}

//定时器回调函数，删除非活动连接在socket上的注册事件，并关闭
void cb_func(client_data *user_data) {
    //删除非活动连接在socket上的注册事件
    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    //关闭文件描述符
    close(user_data->sockfd);
    //减少连接数
    http_conn::m_user_count--;
}

void show_error(int connfd, const char *info) {
    printf("%s", info);
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int main(int argc, char *argv[]) {
    Log::get_instance()->init("ServerLog", 2000, 800000, 8); //异步日志模型
    if (argc <= 1) {
        printf("usage: %s ip_address port_number\n", basename(argv[0]));
        return 1;
    }
    int port = atoi(argv[1]);
    //创建数据库连接池
    connection_pool *connPool = connection_pool::GetInstance();
    connPool->init("localhost", "laifu", "laifu", "webdb", 3306, 8);

    //创建线程池
    threadpool<http_conn> *pool = NULL;
    try {
        pool = new threadpool<http_conn>(connPool);
    }
    catch (...) {
        return 1;
    }
    http_conn *users = new http_conn[MAX_FD];//文件描述符就对应在数组中的位置
    //assert(users);
    //初始化数据库读取表


    int listenfd = socket(PF_INET, SOCK_STREAM, 0);

    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    int ret = 0;
    int flag = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    ret = bind(listenfd, (struct sockaddr *) &address, sizeof(address));
    ret = listen(listenfd, 5);

    //创建内核事件表
    int epollfd = epoll_create(100);
    http_conn::m_epollfd = epollfd; //将上述epollfd赋值给http类对象的m_epollfd属性
    epoll_event events[MAX_EVENT_NUMBER];
    addfd(epollfd, listenfd, false); //将listenfd放在epoll树上

    //创建管道套接字
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    //设置管道写端为非阻塞，为什么写端要非阻塞？send是将信息发送给套接字缓冲区，
    //如果缓冲区满了，则会阻塞，这时候会进一步增加信号处理函数的执行时间，为此，将其修改为非阻塞
    setnonblocking(pipefd[1]);
    //设置管道读端为ET非阻塞
    addfd(epollfd, pipefd[0], false);
    //传递给主循环的信号值，这里只关注SIGALRM和SIGTERM
    addsig(SIGALRM, sig_handler, false);
    addsig(SIGTERM, sig_handler, false);
    bool timeout = false;
    //每隔TIMESLOT时间触发SIGALRM信号
    alarm(TIMESLOT);
    //创建连接资源数组
    client_data *users_timer = new client_data[MAX_FD];

    bool stop_server = false;

    while (!stop_server) {
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);//-1代表阻塞
        if (number < 0 && errno != EINTR) {
            LOG_ERROR("%s", "epoll failure");
            break;
        }
        for (int i = 0; i < number; i++) {
            int sockfd = events[i].data.fd;
            if (sockfd == listenfd) {
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
                while (1) {
                    int connfd = accept(listenfd, (struct sockaddr *) &client_address, &client_addrlength);
                    if (connfd < 0) {
                        LOG_ERROR("%s:errno is:%d", "accept error", errno);
                        break;
                    }
                    if (http_conn::m_user_count >= MAX_FD) {
                        show_error(connfd, "Internal server busy");
                        LOG_ERROR("%s", "Internal server busy");
                        break;
                    }
                    users[connfd].init(connfd, client_address);
                    //初始化该连接对应的连接资源
                    users_timer[connfd].address = client_address;
                    users_timer[connfd].sockfd = connfd;
                    //创建定时器临时变量
                    util_timer *timer = new util_timer;
                    //设置定时器对应的连接资源
                    timer->user_data = &users_timer[connfd];
                    //设置回调函数
                    timer->cb_func = cb_func;
                    time_t cur = time(NULL);
                    //设置绝对超时时间
                    timer->expire = cur + 3 * TIMESLOT;
                    //创建该连接对应的定时器，初始化为前述临时变量
                    users_timer[connfd].timer = timer;
                    //将该定时器添加到链表中
                    timer_lst.add_timer(timer);
                }
                continue;
            }
                //处理异常事件
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                //服务器端关闭连接，移除对应的定时器
                util_timer *timer = users_timer[sockfd].timer;
                timer->cb_func(&users_timer[sockfd]);

                if (timer) {
                    timer_lst.del_timer(timer);
                }
            }
                //处理定时器信号
            else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN)) {
                int sig;
                char signals[1024];
                //从管道读端读出信号值，成功返回字节数，失败返回-1
                //正常情况下，这里的ret返回值总是1，只有14和15两个ASCII码对应的字符
                //信号本身是整型数值,管道中传递的是ASCII码表中整型数值对应的字符。
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if (ret == -1) {
                    // handle the error
                    continue;
                } else if (ret == 0) {
                    continue;
                } else {
                    //处理信号值对应的逻辑
                    for (int i = 0; i < ret; ++i) {
                        //这里面是字符
                        switch (signals[i]) {
                            case SIGALRM: {
                                //接收到SIGALRM信号，timeout设置为True
                                timeout = true;
                                break;
                            }
                                //这里是整型
                            case SIGTERM: {
                                stop_server = true;
                            }
                        }
                    }
                }
            }
                //处理客户连接上接收到的数据
            else if (events[i].events & EPOLLIN) {
                util_timer *timer = users_timer[sockfd].timer;
                //读入对应缓冲区
                if (users[sockfd].read_once()) {
                    LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();
                    //若监测到读事件，将该事件放入请求队列
                    pool->append(users + sockfd);
                    //若有数据传输，则将定时器往后延迟3个单位
                    //并对新的定时器在链表上的位置进行调整
                    if (timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        LOG_INFO("%s", "adjust timer once");
                        Log::get_instance()->flush();
                        timer_lst.adjust_timer(timer);
                    }
                } else {
                    //服务器关闭连接，移除对应的定时器
                    timer->cb_func(&users_timer[sockfd]);
                    if (timer)
                    {
                        timer_lst.del_timer(timer);
                    }
                }
            } else if (events[i].events & EPOLLOUT) {
                util_timer *timer = users_timer[sockfd].timer;
                if (users[sockfd].write()) {
                    LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();

                    //若有数据传输，则将定时器往后延迟3个单位
                    //并对新的定时器在链表上的位置进行调整
                    if (timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        LOG_INFO("%s", "adjust timer once");
                        Log::get_instance()->flush();
                        timer_lst.adjust_timer(timer);
                    }
                } else {
                    //服务器端关闭连接，移除对应的定时器
                    timer->cb_func(&users_timer[sockfd]);
                    if (timer)
                    {
                        timer_lst.del_timer(timer);
                    }
                }
            }
        }
        //处理定时器为非必须事件，收到信号并不是立马处理
        //完成读写事件后，再进行处理
        if (timeout)
        {
            timer_handler();
            timeout = false;
        }
    }
    close(epollfd);
    close(listenfd);
    close(pipefd[1]);
    close(pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete pool;
    return 0;
}