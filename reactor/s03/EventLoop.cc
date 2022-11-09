// excerpts from http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.
//
// Author: Shuo Chen (chenshuo at chenshuo dot com)

#include "EventLoop.h"

#include "Channel.h"
#include "Poller.h"
#include "TimerQueue.h"

#include "logging/Logging.h"

#include <boost/bind.hpp>

#include <assert.h>
#include <sys/eventfd.h>

using namespace muduo;

__thread EventLoop *t_loopInThisThread = 0;
const int kPollTimeMs = 10000;

static int createEventfd()
{
  int evtfd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (evtfd < 0)
  {
    LOG_SYSERR << "Failed in eventfd";
    abort();
  }
  return evtfd;
}

EventLoop::EventLoop()
    : looping_(false),
      quit_(false),
      callingPendingFunctors_(false),
      threadId_(CurrentThread::tid()),
      poller_(new Poller(this)),
      timerQueue_(new TimerQueue(this)),
      wakeupFd_(createEventfd()),
      wakeupChannel_(new Channel(this, wakeupFd_))
{
  LOG_TRACE << "EventLoop created " << this << " in thread " << threadId_;
  if (t_loopInThisThread)
  {
    LOG_FATAL << "Another EventLoop " << t_loopInThisThread
              << " exists in this thread " << threadId_;
  }
  else
  {
    t_loopInThisThread = this;
  }

  //注册wakeupchannel_,设置读回调（也就是读一个字节）
  wakeupChannel_->setReadCallback(
      boost::bind(&EventLoop::handleRead, this));
  // we are always reading the wakeupfd
  wakeupChannel_->enableReading();
}

EventLoop::~EventLoop()
{
  assert(!looping_);
  ::close(wakeupFd_);
  t_loopInThisThread = NULL;
}

void EventLoop::loop()
{
  assert(!looping_);
  assertInLoopThread();
  looping_ = true;
  quit_ = false;

  while (!quit_)
  {
    activeChannels_.clear();
    pollReturnTime_ = poller_->poll(kPollTimeMs, &activeChannels_);
    for (ChannelList::iterator it = activeChannels_.begin();
         it != activeChannels_.end(); ++it)
    {
      (*it)->handleEvent();
    }

    //在poll之后，doPendingFunctors之前调用queueInLoop()，才无需wakeup唤醒
    doPendingFunctors();
  }

  LOG_TRACE << "EventLoop " << this << " stop looping";
  looping_ = false;
}

void EventLoop::quit()
{
  quit_ = true;
  if (!isInLoopThread())
  {
    wakeup();
  }
}

void EventLoop::runInLoop(const Functor &cb)
{
  //如果在当前IO的线程,就直接执行回调
  if (isInLoopThread())
  {
    cb();
  }
  else //不在就加入队里当中，然后唤醒IO线程去执行该回调函数
  {
    queueInLoop(cb);
  }
}

void EventLoop::queueInLoop(const Functor &cb)
{
  //因为该调用可能在当前IO线程，为了确保线程安全吗，所以要加锁保护
  {
    MutexLockGuard lock(mutex_);
    pendingFunctors_.push_back(cb);
  }

  //在必要时唤醒IO线程
  //必要 ：1.不在IO线程调用queueInLoop();2.该IO线程正在执行pendinng functor
  if (!isInLoopThread() || callingPendingFunctors_)
  {
    wakeup();
  }
}

TimerId EventLoop::runAt(const Timestamp &time, const TimerCallback &cb)
{
  return timerQueue_->addTimer(cb, time, 0.0);
}

TimerId EventLoop::runAfter(double delay, const TimerCallback &cb)
{
  Timestamp time(addTime(Timestamp::now(), delay));
  return runAt(time, cb);
}

TimerId EventLoop::runEvery(double interval, const TimerCallback &cb)
{
  Timestamp time(addTime(Timestamp::now(), interval));
  return timerQueue_->addTimer(cb, time, interval);
}

void EventLoop::updateChannel(Channel *channel)
{
  assert(channel->ownerLoop() == this);
  assertInLoopThread();
  poller_->updateChannel(channel);
}

void EventLoop::abortNotInLoopThread()
{
  LOG_FATAL << "EventLoop::abortNotInLoopThread - EventLoop " << this
            << " was created in threadId_ = " << threadId_
            << ", current thread id = " << CurrentThread::tid();
}

void EventLoop::wakeup()
{
  uint64_t one = 1;
  ssize_t n = ::write(wakeupFd_, &one, sizeof one);
  if (n != sizeof one)
  {
    LOG_ERROR << "EventLoop::wakeup() writes " << n << " bytes instead of 8";
  }
}

void EventLoop::handleRead()
{
  uint64_t one = 1;
  ssize_t n = ::read(wakeupFd_, &one, sizeof one);
  if (n != sizeof one)
  {
    LOG_ERROR << "EventLoop::handleRead() reads " << n << " bytes instead of 8";
  }
}

void EventLoop::doPendingFunctors()
{
  std::vector<Functor> functors;
  callingPendingFunctors_ = true;

  //先将回调任务swap到局部变量中
  //原因： 1.防止阻塞其他进程
  //      2.防止死锁(在回调任务当中可能也会调用queueInLoop()函数)
  {
    MutexLockGuard lock(mutex_);
    functors.swap(pendingFunctors_);
  }

  for (size_t i = 0; i < functors.size(); ++i)
  {
    functors[i]();
  }
  callingPendingFunctors_ = false;
}
