#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cm {

// 固定线程数的任务池：构造即启动；stop() 拒绝新任务、跑完队列后退出。
class ThreadPool {
 public:
  using Task = std::function<void()>;

  explicit ThreadPool(size_t threadCount = 4, const std::string& name = "threadpool");
  ~ThreadPool();

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  void start();

  // 停机：之后 run() 返回 false；已在队列中的任务会执行完（析构不丢任务）
  void stop();

  bool run(Task task);

  size_t threadCount() const { return threadCount_; }
  size_t pendingTasks() const;
  const std::string& name() const { return name_; }

 private:
  void workerLoop();

  mutable std::mutex mutex_;
  std::condition_variable cond_;
  std::deque<Task> tasks_;
  std::vector<std::thread> threads_;
  std::string name_;
  size_t threadCount_;
  bool started_;
  bool stopping_;
};

}  // namespace cm
