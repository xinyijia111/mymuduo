#include "EventLoop.h"
#include "Logger.h"
#include "Poller.h"
#include "Channel.h"

#include <sys/eventfd.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno>

// 防止一个线程创建多个EventLoop
__thread EventLoop *t_loopInThisThread = nullptr;

// 定义默认的Poller I/O复用接口的超时时间
const int kPollTimeMs = 10000;

// 创建wakeupfd, 用来notify subReactor处理新来的channel
int createEventfd()
{
    int evtfd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if(evtfd < 0)
    {
        LOG_FATAL("eventfd errno:%d \n", errno);
    }
    return evtfd;
}

EventLoop::EventLoop()
  : looping_(false)
  , quit_(false)
  , callingPendingFunctors_(false)
  , threadId_(CurrentThread::tid())
  , poller_(Poller::newDefaultPoller(this))
  , wakeupFd_(createEventfd())
  , wakeupChannel_(new Channel(this, wakeupFd_))
  {
    LOG_DEBUG("EventLoop created %p in thread %d", this, threadId_);
    if(t_loopInThisThread)
    {
        LOG_FATAL("Another EventLoop %p exists in this thread %d \n", t_loopInThisThread, threadId_);
    }
    else
    {
        t_loopInThisThread = this;
    }

    //设置wakeupfd的事件类型以及发生事件后的回调操作
    wakeupChannel_->setReadCallback(std::bind(&EventLoop::handleRead, this));
    // 每一个eventloop都将监听wakeupchannel的EPOLLIN读事件了
    wakeupChannel_->enableReading();
  }

EventLoop::~EventLoop()
{
    wakeupChannel_->disableAll();
    wakeupChannel_->remove();
    ::close(wakeupFd_);
    t_loopInThisThread = nullptr;
}

void EventLoop::handleRead()
{
    uint64_t one = 1;
    ssize_t n = read(wakeupFd_, &one, sizeof one);
    if(n != sizeof one)
    {
        LOG_ERROR("EventLoop::handleRead() reads %d bytes instead of 8", n);
    }
}

//开启事件循环
void loop()
{
    looping_ = true;
    quit_ = false;

    LOG_INFO("EventLoop %p start looping", this);

    while(!quit_)
    {
        activeChannels_.clear();
        /**
         * subloop会阻塞在这里，我们通过eventfd,得到一个wakeupfd,handle是读取一个8字节的数据
         * 成功把subloop唤醒
        */
       /**
        * 监听两类fd  一种是client的fd，一种是wakefd
       */
        pollReturnTime_ = poller_->poll(kPollTimeMs, &activeChannels_);
        for(Channel* channel : activeChannels_)
        {
            // poller监听哪些channel发生事件了，然后上报给EventLoop，通知channel处理相应的事件
            channel->handleEvent(pollReturnTime_);
        }

        // 执行当前EventLoop事件循环需要处理的回调操作
        /**
         * I/O线程 mainLoop accept fd => channel  subloop
         * mainLoop事先注册一个回调cb(需要subloop来执行) 
         * 唤醒subloop后，执行下面的方法，执行之前mainLoop注册的回调
        */
        doPendingFunctors();
    }

    LOG_INFO("EventLoop %p stop looping.", this)
    looping_ = false;
}

//退出事件循环
/**
 * 1. loop在自己的线程中调用quit
 * 2. 在非loop的线程中，调用loop的quit
*/
/**
 *                   mainLoop
 * 
 *       =========================生产者-消费者的线程安全的队列
 * 
 * subLoop1        subLoop2         subLoop3  
 * 
 * muduo中mainLoop和subLoop之间通信，就是通过wakeupFd_，来直接进行notify
 * 但是如果在mainLoop和subLoop中间加一个生产者-消费者的线程安全队列的话，也是可以的 
*/
void EventLoop::quit()
{
    quit_ = true;
    // 如果是在其他线程中，调用的quit  在subloop中，调用了mainLoop的quit
    // 1. 唤醒它，它从poll解除阻塞，在loop()循环中，因为quit == true, 跳出循环
    // 2. 对方正在忙，结束后，在poll处阻塞……
    if(!isInLoopThread())
    {
        wakeup();
    }
}

//在当前loop中执行cb
void EventLoop::runInLoop(Functor cb)
{
    if(isInLoopThread()) // loop在当前的loop线程中
    {
        cb();
    } 
    else //在非当前loop线程中执行cb，就需要唤醒loop所在线程，执行cb
    {
        queueInLoop(cb);
    }
}


//把cb放入队列中，唤醒loop所在线程，执行cb
void EventLoop::queueInLoop(Functor cb)
{
    {
        std::unique_lock<std::mutex> lock(mtx_);
        pendingFunctors_.emplace_back(cb);
    }

    // 唤醒相应的，需要执行上面回调操作的loop的线程了
    // callingPendingFunctors_ == true, 表示正在执行回调，但是loop又有了新的回调。当执行完回调后，又会poll阻塞，我们是需要去唤醒它的
    if(!isInLoopThread() || callingPendingFunctors_)
    {
        wakeup(); // 唤醒loop所在线程
    }
}

 //唤醒loop所在的线程的
 // 向wakeupFd写数据,wakeupChannel就发生读事件，当前loop线程就会被唤醒
void EventLoop::wakeup() // main 唤醒 sub
{
    uint64_t one = 1;
    ssize_t n = write(wakeupFd_, &one, sizeof one);
    if(n != sizeof one)
    {
        LOG_ERROR("EventLoop:wakeup() writes %lu bytes instead of 8", n);
    }
}

// 调用poller方法
void EventLoop::updateChannel(Channel* channel)
{
    poller_->updateChannel(channel);
}
void EventLoop::removeChannel(Channel* channel)
{
    poller_->removeChannel(channel);
}
bool EventLoop::hasChannel(Channel* channel)
{
    poller_->hasChannel(channel);
}

// 执行回调
void EventLoop::doPendingFunctors()
{
    /**
     * 局部变量functors, 进行swap
     * 避免了频繁的加锁释放锁
     * 【mainLoop也需要往pendingFunctors_中放回调】
    */
    std::vector<Functor> functors;
    callingPendingFunctors_ = true;

    {
        std::unqiue_lock<std::mutex> lock(mtx_);
        functors.swap(pendingFunctors_);
    }

    for(const Functor& functor : functors)
    {
        functor(); // 执行当前loop要执行的回调操作
    }
    callingPendingFunctors_ = false;
}