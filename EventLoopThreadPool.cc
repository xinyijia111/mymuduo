#include "EventLoopThreadPool.h"
#include "EventLoopThread.h"

EventLoopThreadPool::EventLoopThreadPool(EventLoop *baseLoop, const std::string &nameArg)
    : baseLoop_(baseLoop)
    , name_(nameArg)
    , started_(false)
    , numThreads_(0)
    , next_(0)
    {}

EventLoopThreadPool::~EventLoopThreadPool()
{
    // 创建的loop都是栈上的对象，不用delete
}

void EventLoopThreadPool::start(const ThreadInitCallback &cb)
{
    started_ = true;

    for(int i = 0;i < numThreads_;i++)
    {
        char buf[name_.size() + 32];
        snprintf(buf, sizeof buf, "%s%d", name.c_str(), i);
        EventLoopThread *t = new EventLoopThread(cb, buf);
        threads_.push_back(std::unique_ptr<EventLoopThread(t)>);
        loops_.push_back(t->startLoop()); // 底层创建线程，绑定一个新的EventLoop,并返回该loop的地址
    }

    // 整个服务端只有一个线程，运行着baseLoop
    if(numThreads_ == 0 && cb)
    {
        cb(baseLoop_);
    }
}

// 如果工作在多线程中，baseLoop_默认以轮询的方式分配channel给subloop
EventLoop* EventLoopThreadPool::getNextLoop()
{
    EventLoop* loop = baseLoop_;
    if(!loops_.empty())
    {
        loop = loops_[next_];
        ++next_;
        if(next_ >= loops_.size()) next_ = 0; 
    }
    return loop;
}

std::vector<EventLoop*> EventLoopThreadPool::getAllLoops()
{
    if(loops_.empty()) // 没有创建过loop
    {
        return std::vector<EventLoop*>(1, baseLoop_);
    }
    return loops_;
}