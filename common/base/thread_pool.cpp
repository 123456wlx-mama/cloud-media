#include "cm/base/thread_pool.h"

#include <utility>

namespace cm {

ThreadPool::ThreadPool(size_t threadCount, const std::string& name)
    : name_(name), threadCount_(threadCount == 0 ? 1 : threadCount), started_(false), stopping_(false) {
  start();
}

ThreadPool::~ThreadPool() { stop(); }

void ThreadPool::start() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (started_) {
    return;
  }
  started_ = true;
  threads_.reserve(threadCount_);
  for (size_t i = 0; i < threadCount_; ++i) {
    threads_.push_back(std::thread(&ThreadPool::workerLoop, this));
  }
}

void ThreadPool::stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) {
      return;
    }
    stopping_ = true;
  }
  cond_.notify_all();

  for (size_t i = 0; i < threads_.size(); ++i) {
    if (threads_[i].joinable()) {
      threads_[i].join();
    }
  }
  threads_.clear();
}

bool ThreadPool::run(Task task) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_ || stopping_) {
      return false;
    }
    tasks_.push_back(std::move(task));
  }
  cond_.notify_one();
  return true;
}

size_t ThreadPool::pendingTasks() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return tasks_.size();
}

void ThreadPool::workerLoop() {
  for (;;) {
    Task task;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cond_.wait(lock, [this]() { return stopping_ || !tasks_.empty(); });
      // 停机也要把队列跑空，避免析构丢任务
      if (tasks_.empty()) {
        if (stopping_) {
          return;
        }
        continue;
      }
      task = std::move(tasks_.front());
      tasks_.pop_front();
    }
    task();
  }
}

}  // namespace cm
