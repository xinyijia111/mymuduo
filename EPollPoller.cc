#include "EPollPoller.h"
#include "Logger.h"

#include <errno.h>
#include <Channel.h>
#include <unistd.h>
#include <strings.h>

// channel的成员index_ = -1
const int KNew = -1; // channel没有添加到过epoll中
const int KAdded = 1; // channel已经添加到epoll中
const int KDeleted = 2; // channel在epoll中删除了


EPollPoller::EPollPoller(EventLoop *loop)
    : Poller(loop)  // 调用基类的构造，来初始化继承来的对象
    , epollfd_(::epoll_create1(EPOLL_CLOEXEC))
    , events_(KInitEventListSize)
{
    if(epollfd_ < 0)
    {
        LOG_FATAL("epoll_create error:%d\n", errno);
    }
}

EPollPoller::~EPollPoller()
{
    ::close(epollfd_);
}

//通过epoll_wait监听到哪些fd发生事件，把发生事件的fd放到activeChannels中，传递给EventLoop
// activeChannels是一个传出参数
// 调用epoll_wait, 针对不同情况，进行处理
Timestamp EPollPoller::poll(int timeoutMs, ChannelList *activeChannels)
{
    // 实际上应该用LOG_DEBUG输出日志更为合理
    LOG_INFO("func=%s => fd total count:%lu\n", __FUNCTION__, channels_.size());

    // 第二个参数要给数组的起始地址
    int numEvents = ::epoll_wait(epollfd, &*events_.begin(), static_cast<int>(events_.size()), timeoutMs);
    int savedErrno = errno;
    Timestamp now(Timestamp::now());

    if(numEvents > 0) //有发生事件fd：fd个数
    {
        LOG_INFO("%d events happened.\n", numEvents);
        fillActiveChannels(numEvents, activeChannels);

        if(numEvents == events_.size()) //扩容
        {
            events_.resize(events_.size() * 2);
        }        
    }
    else if(numEvents == 0)
    {
        LOG_DEBUG("%s timeout!\n", __FUNCTION__);
    }
    else
    {
        if(saveError != EINTR) //不是外部中断
        {
            errno = savedErrno;
            LOG_ERROR("EPollPoller::poll() err!");
        }
    }

    return now;
}

// channel update remove => EventLoop updateChannel removeChannel => Poller
/*       EventLoop
 *  ChannelList     Poller
 *                  ChannelMap  <fd, channel*>
*/
void EPollPoller::updateChannel(Channel *channel)
{
    const int index = channel->index();
    LOG_INFO("func=%s => fd=%d, events=%d, index=%d \n",__FUNCTION__, channel->fd(), channel->events(), index);
    if(index == KNew || index == KDeleted)
    {
        if(index == KNew)
        {
            int fd = channel->fd();
            channels_[fd] = channel;
        }

        channel->set_index(KAdded);
        update(EPOLL_CTL_ADD, channel);
    }
    else // channel已经在poller上注册过了
    {
        int fd = channel->fd();
        if(channel->isNoneEvent())
        {
            update(EPOLL_CTL_DEL, channel);
        }
        else
        {
            update(EPOLL_CTL_MOD, channel);
        }
    }
}

//从poller中删除channel
void EPollPoller::removeChannel(Channel *channel)
{
    int fd = channel->fd();
    int index = channel->index();

    LOG_INFO("func=%s => fd=%d\n",__FUNCTION__, fd);

    channels_.erase(fd);

    if(index == KAdded)
    {
        update(EPOLL_CTL_DEL, channel);
    }
    channel->set_index(KNew);
}

// 填写活跃的连接
/**
 * event.data.ptr = channel;
*/
void EPollPoller::fillActiveChannels(int numEvents, ChannelList *activeChannels) const
{
    for(int i = 0;i < numEvents;i++)
    {
        Channel *channel = static_cast<Channel*>(events_[i].data.ptr);
        channel->set_revents(events_[i].events);
        activeChannels->push_back(channel);
    }
}

// 更新channel通道
/**
 * epoll_event 里面的 event_data_t data 成员
 * 
 * event_data_t  结构体里面：fd  ptr ……
*/
void EPollPoller::update(int operation, Channel *channel)
{
    epoll_event event;
    bzero(&event, sizeof(event));
    event.events = channel->events();
    event.data.ptr = channel;
    int fd = channel->fd();
    event.data.fd = fd;
    if(::epoll_ctl(epollfd_, operation, fd, &event) < 0)
    {
        if(operation == EPOLL_CTL_DEL)
        {
            LOG_ERROR("epoll_ctl del error:%d\n", errno);
        }
        else 
        {
            LOG_FATAL("epoll_ctl add/mode error:%d\n", errno);
        }
    }
}