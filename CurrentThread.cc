#include "CurrentThread.h"

namespace CurrentThread
{
    // t_cachedTid是全局变量，加了__thread修饰之后，每个线程中的t_cachedTid都不同
    __thread int t_cachedTid;

    void cacheTid()
    {
        if(t_cachedTid == 0)
        {
            // 通过linux系统调用，获取当前线程的tid值
            t_cachedTid = static_cast<pid_t>(::syscall(SYS_gettid));
        }
    }
}