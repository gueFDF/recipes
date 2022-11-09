// excerpts from http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.
//
// Author: Shuo Chen (chenshuo at chenshuo dot com)

#ifndef MUDUO_NET_EVENTLOOP_H
#define MUDUO_NET_EVENTLOOP_H

#include "datetime/Timestamp.h"
#include "thread/Mutex.h"
#include "thread/Thread.h"
#include "Callbacks.h"
#include "TimerId.h"

#include <boost/scoped_ptr.hpp>
#include <vector>

namespace muduo
{

class Channel;
class Poller;
class TimerQueue;

class EventLoop : boost::noncopyable
{
 public:
  typedef boost::function<void()> Functor;

  EventLoop();

  // force out-line dtor, for scoped_ptr members.
  ~EventLoop();

  ///
  /// Loops forever.
  ///
  /// Must be called in the same thread as creation of the object.
  ///
  void loop();

  void quit();

  ///
  /// Time when poll returns, usually means data arrivial.
  ///
  Timestamp pollReturnTime() const { return pollReturnTime_; }

  /// Runs callback immediately in the loop thread.
  /// It wakes up the loop, and run the cb.
  /// If in the same loop thread, cb is run within the function.
  /// Safe to call from other threads.
  
  //立刻执行回调任务，如果是在其他线程调用该函数，回调任务会被加入队列当中，IO线程会被唤醒，执行该回调
  void runInLoop(const Functor& cb);
  /// Queues callback in the loop thread.
  /// Runs after finish pooling.
  /// Safe to call from other threads.

  //将任务cb放入队列，并在必要时唤醒IO线程
  void queueInLoop(const Functor& cb);

  // timers

  ///
  /// Runs callback at 'time'.
  /// Safe to call from other threads.
  ///
  TimerId runAt(const Timestamp& time, const TimerCallback& cb);
  ///
  /// Runs callback after @c delay seconds.
  /// Safe to call from other threads.
  ///
  TimerId runAfter(double delay, const TimerCallback& cb);
  ///
  /// Runs callback every @c interval seconds.
  /// Safe to call from other threads.
  ///
  TimerId runEvery(double interval, const TimerCallback& cb);

  // void cancel(TimerId timerId);

  // internal use only

  //唤醒机制就是，eventloop创建时，会注册一个wakefd_，该文件描述符始终关注读事件
  //当执行wakeup()函数时，会向wakefd_写入一个字符数据，原本阻塞在poll的io线程就会被唤醒
  //执行所触发事件的文件描述符所绑定的回调函数，wakefd_绑定的回调函数是handleRead()（读一个字节）
  //该回调的作用仅仅只是将wakeup()写入的一个字符数据读出。在poll循环中最后会执行
  //doPendingFunctors函数执行所有等待队列中的函数任务
  void wakeup();
  void updateChannel(Channel* channel);
  // void removeChannel(Channel* channel);

  void assertInLoopThread()
  {
    if (!isInLoopThread())
    {
      abortNotInLoopThread();
    }
  }

  bool isInLoopThread() const { return threadId_ == CurrentThread::tid(); }

 private:

  void abortNotInLoopThread();
  void handleRead();  // waked up
  void doPendingFunctors();

  typedef std::vector<Channel*> ChannelList;

  bool looping_; /* atomic */
  bool quit_; /* atomic */

  //该字段是用来标识是否正在处理pendingFunctors_(),从而判断wakeup时机，若正在执行pendingFunctors_则w需要akeup
  bool callingPendingFunctors_; /* atomic */
  const pid_t threadId_;
  Timestamp pollReturnTime_;
  boost::scoped_ptr<Poller> poller_;
  boost::scoped_ptr<TimerQueue> timerQueue_;
  int wakeupFd_;
  // unlike in TimerQueue, which is an internal class,
  // we don't expose Channel to client.

  //该channel用于处理wakeup_上的readable事件，将事件分发至handleRead()函数处理
  boost::scoped_ptr<Channel> wakeupChannel_;
  ChannelList activeChannels_;

  //pendingFunctors_会暴露给其他线程，需要mutex的保护
  MutexLock mutex_;

  //未执行的任务队列
  std::vector<Functor> pendingFunctors_; // @GuardedBy mutex_
};

}

#endif  // MUDUO_NET_EVENTLOOP_H
