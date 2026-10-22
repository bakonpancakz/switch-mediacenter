#ifndef BELL_TASK_H
#define BELL_TASK_H

#include <string>

#ifdef ESP_PLATFORM
#include <esp_pthread.h>
#include <esp_task.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/timers.h>
#elif _WIN32
#include <winsock2.h>
#else
#include <pthread.h>
#endif
#if defined(__SWITCH__)
#include <switch.h>
#endif

#include <atomic>
#include <iostream>
#include <string>

namespace bell {
class Task {
 public:
  std::string TASK;
  int stackSize, core;
  bool runOnPSRAM;
  Task(std::string taskName, int stackSize, int priority, int core,
       bool runOnPSRAM = true) {
    this->TASK = taskName;
    this->stackSize = stackSize;
    this->core = core;
    this->runOnPSRAM = runOnPSRAM;
#ifdef ESP_PLATFORM
    this->xStack = NULL;
    this->priority = CONFIG_ESP32_PTHREAD_TASK_PRIO_DEFAULT + priority;
    if (this->priority <= ESP_TASK_PRIO_MIN)
      this->priority = ESP_TASK_PRIO_MIN + 1;
    if (runOnPSRAM) {
      this->xStack = (StackType_t*)heap_caps_malloc(
          this->stackSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
#endif
  }
  virtual ~Task() {
#ifdef ESP_PLATFORM
    if (xStack)
      heap_caps_free(xStack);
#endif
#if defined(__SWITCH__)
    // Derived destructors stop runTask first, so the thread is (about to be) done.
    joinPrevious();
#endif
  }

  bool startTask() {
#ifdef ESP_PLATFORM
    if (runOnPSRAM) {
      xTaskBuffer = (StaticTask_t*)heap_caps_malloc(
          sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      return (xTaskCreateStaticPinnedToCore(
                  taskEntryFuncPSRAM, this->TASK.c_str(), this->stackSize, this,
                  this->priority, xStack, xTaskBuffer, this->core) != NULL);
    } else {
      printf("task on internal %s", this->TASK.c_str());
      esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
      cfg.stack_size = stackSize;
      cfg.inherit_cfg = true;
      cfg.thread_name = this->TASK.c_str();
      cfg.pin_to_core = core;
      cfg.prio = this->priority;
      esp_pthread_set_cfg(&cfg);
    }
#endif
#if _WIN32
    thread = CreateThread(NULL, stackSize,
                          (LPTHREAD_START_ROUTINE)taskEntryFunc, this, 0, NULL);
    return thread != NULL;
#elif defined(__SWITCH__)
    // Switchify: ESP-sized stacks are too small for mbedtls/vorbis on A57
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    size_t size = stackSize * 3 / 2;
    if (size < 32 * 1024)
      size = 32 * 1024;
    pthread_attr_setstacksize(&attr, (size + 0xFFF) & ~0xFFF);
    // MediaCenter: tasks are joined, never detached. libnx doesn't support
    // detaching (its thread_detach is a stub), so a detached thread's stack stays
    // mapped after it ends, and the launcher (sphaira's hbl) then can't reuse
    // that memory on exit: a crash with 0xD401. A restarted task (TrackPlayer
    // is stopped and started again) first joins its previous thread.
    joinPrevious();
    finished = false;
    int err = pthread_create(&thread, &attr, taskEntryFunc, this);
    pthread_attr_destroy(&attr);
    if (!err) {
      joinable = true;
      return true;
    }
    return false;
#else
    if (!pthread_create(&thread, NULL, taskEntryFunc, this)) {
      pthread_detach(thread);
      return true;
    }
    return false;
#endif
  }

 protected:
  virtual void runTask() = 0;

 private:
#if _WIN32
  HANDLE thread;
#else
  pthread_t thread;
#endif
#if defined(__SWITCH__)
  bool joinable = false;
  std::atomic<bool> finished{false};
  void joinPrevious() {
    if (!joinable) return;
    joinable = false;
    // runTask normally ends within moments of being told to stop; never hang on it
    for (int i = 0; i < 300 && !finished; i++) svcSleepThread(10000000ULL);
    if (finished) pthread_join(thread, NULL);
    else {
      fprintf(stderr, "bell: task %s still running, detaching\n", TASK.c_str());
      pthread_detach(thread);
    }
  }
#endif
#ifdef ESP_PLATFORM
  int priority;
  StaticTask_t* xTaskBuffer;
  StackType_t* xStack;

  static void taskEntryFuncPSRAM(void* This) {
    Task* self = (Task*)This;
    self->runTask();

    // TCB are cleanup in IDLE task, so give it some time
    TimerHandle_t timer =
        xTimerCreate("cleanup", pdMS_TO_TICKS(5000), pdFALSE, self->xTaskBuffer,
                     [](TimerHandle_t xTimer) {
                       heap_caps_free(pvTimerGetTimerID(xTimer));
                       xTimerDelete(xTimer, portMAX_DELAY);
                     });
    xTimerStart(timer, portMAX_DELAY);

    vTaskDelete(NULL);
  }
#endif

  static void* taskEntryFunc(void* This) {
#if defined(__SWITCH__)
    // MediaCenter: off the UI's core (threads start on core 0)
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, 2, 0x7);
#endif
    ((Task*)This)->runTask();
#if defined(__SWITCH__)
    ((Task*)This)->finished = true;
#endif
    return NULL;
  }
};
}  // namespace bell

#endif
