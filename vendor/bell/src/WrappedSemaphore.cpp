#include "WrappedSemaphore.h"

#include <chrono>

using namespace bell;

WrappedSemaphore::WrappedSemaphore(int maxVal) : maxCount(maxVal) {}

WrappedSemaphore::~WrappedSemaphore() {}

int WrappedSemaphore::wait() {
  std::unique_lock<std::mutex> lock(mutex);
  cv.wait(lock, [this] { return count > 0; });
  count--;
  return 0;
}

// Returns 0 when taken, non-zero on timeout (matches sem_timedwait semantics)
int WrappedSemaphore::twait(long milliseconds) {
  std::unique_lock<std::mutex> lock(mutex);
  if (!cv.wait_for(lock, std::chrono::milliseconds(milliseconds),
                   [this] { return count > 0; })) {
    return 1;
  }
  count--;
  return 0;
}

void WrappedSemaphore::give() {
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (count < maxCount)
      count++;
  }
  cv.notify_one();
}
