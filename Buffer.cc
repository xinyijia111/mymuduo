#include "Buffer.h"

#include <errno.h>
#include <sys/uio.h>
#include <unistd.h>
/**
 * 从fd上读取数据  Poller工作在LT模式
 * Buffer缓冲区是有大小的。但是从fd上读取数据的时候，是不知道tcp数据最终的大小
*/
/**
 * readv() 系统调用！！！！！！！！！
*/
ssize_t Buffer::readFd(int fd, int* saveErrno)
{
    char extrabuf[65536] = {0}; //栈上的内存空间  64k
    struct iovec vec[2];
    const size_t writable = writeableBytes(); // Buffer底层缓冲区剩余的可写空间大小
    vec[0].iov_base = begin() + writerIndex_;
    vec[0].iov_len = writable;

    vec[1].iov_base = extrabuf;
    vec[1].iov_len = sizeof extrabuf;

    const int iovcnt = (writeable < sizeof extrabuf) ? 2 : 1;
    const ssize_t n = ::readv(fd, &vec, iovcnt);
    if(n < 0)
    {
        *saveErrno = errno;
    }
    else if(n <= writeable) // Buffer可写缓冲区够放读出来的数据
    {
        writerIndex_ += n;
    }
    else // extrabuf中也写入了数据
    {
        writerIndex_ = buffer_.size();
        append(extrabuf, n-writable);
    }

    *saveErrno = errno;
    return n;
}

ssize_t Buffer::writeFd(int fd, int* saveErrno)
{
    // 把outputBuffer_可读区域的数据写到fd中
    ssize_t n = ::write(fd, peek(), readableBytes());
    if(n < 0)
    {
        *saveErrno = errno;
    }
    return n;
}