#pragma once

// Switchify: portable counting semaphore (newlib on Switch has no sem_timedwait)
#include <condition_variable>
#include <mutex>

namespace bell {

class WrappedSemaphore {
 private:
  std::mutex mutex;
  std::condition_variable cv;
  int count = 0;
  int maxCount;

 public:
  WrappedSemaphore(int maxVal = 200);
  ~WrappedSemaphore();

  int wait();
  int twait(long milliseconds = 10);
  void give();
};

}  // namespace bell
