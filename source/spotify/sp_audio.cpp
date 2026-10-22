#include "sp_audio.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "sp_log.hpp"

// Safe to call again after stop(): sleep/wake closes and reopens audout,
// keeping whatever PCM is still in the ring
bool AudioOutput::start() {
  if (running)
    return true;
  Result rc = audoutInitialize();
  if (R_FAILED(rc)) {
    LOG("audoutInitialize failed: 0x%x", rc);
    return false;
  }
  rc = audoutStartAudioOut();
  if (R_FAILED(rc)) {
    LOG("audoutStartAudioOut failed: 0x%x", rc);
    audoutExit();
    return false;
  }
  LOG("audout: %u Hz, %u ch", audoutGetSampleRate(), audoutGetChannelCount());

  for (int i = 0; i < kNumBuffers; i++) {
    bufferMem[i] = aligned_alloc(0x1000, kOutBytes);
    if (!bufferMem[i])
      return false;
    memset(bufferMem[i], 0, kOutBytes);
    buffers[i].next = nullptr;
    buffers[i].buffer = bufferMem[i];
    buffers[i].buffer_size = kOutBytes;
    buffers[i].data_size = kOutFrames * 4;
    buffers[i].data_offset = 0;
  }

  running = true;
  // Same priority as our other threads (0x2C). Anything higher risks starving
  // system services on core 3 if this thread ever misbehaves.
  rc = threadCreate(&thread, threadEntry, this, nullptr, 0x8000, 0x2C, -2);
  if (R_FAILED(rc)) {
    LOG("audio threadCreate failed: 0x%x", rc);
    return false;
  }
  threadStart(&thread);
  return true;
}

void AudioOutput::stop() {
  if (!running)
    return;
  running = false;
  threadWaitForExit(&thread);
  threadClose(&thread);
  audoutStopAudioOut();
  audoutExit();
  for (auto& mem : bufferMem) {
    free(mem);
    mem = nullptr;
  }
}

size_t AudioOutput::writePCM(const uint8_t* data, size_t len, size_t trackHash) {
  std::lock_guard<std::mutex> lock(ringMutex);
  size_t space = kRingBytes - (size_t)(writePos - readPos);
  size_t n = len < space ? len : space;
  n &= ~(size_t)3;  // whole stereo frames only
  if (n == 0)
    return 0;

  if (trackHash != lastHash) {
    markers.push_back({writePos, trackHash});
    lastHash = trackHash;
  }

  size_t off = writePos % kRingBytes;
  size_t first = n < kRingBytes - off ? n : kRingBytes - off;
  memcpy(&ring[off], data, first);
  memcpy(&ring[0], data + first, n - first);
  writePos += n;
  return n;
}

void AudioOutput::clear() {
  std::lock_guard<std::mutex> lock(ringMutex);
  readPos = writePos;
  markers.clear();
  lastHash = 0;
}

bool AudioOutput::popFrame(int16_t& l, int16_t& r) {
  if (writePos - readPos < 4)
    return false;

  while (!markers.empty() && markers.front().pos <= readPos) {
    size_t hash = markers.front().hash;
    markers.pop_front();
    // A seek clears the ring and re-marks the same track; only a real change
    // counts, or cspot would treat it as moving on to the next song
    if (hash != reachedHash) {
      reachedHash = hash;
      if (onTrackReached)
        onTrackReached(hash);
    }
  }

  int16_t frame[2];
  size_t off = readPos % kRingBytes;
  memcpy(frame, &ring[off], 4);  // ring size is a multiple of 4, never splits
  readPos += 4;
  l = frame[0];
  r = frame[1];
  return true;
}

void AudioOutput::fill(int16_t* out, size_t frames) {
  // Linear interpolation from 44.1k to 48k
  constexpr double step = (double)kInputRate / 48000.0;
  float vol = volume;
  bool dry = false;
  eq.begin();
  bool useEq = eq.active();

  std::lock_guard<std::mutex> lock(ringMutex);
  for (size_t i = 0; i < frames; i++) {
    if (paused || dry) {
      out[i * 2] = out[i * 2 + 1] = 0;
      tap.push(0);
      continue;
    }

    int16_t nextL, nextR;
    // Peek next input frame without consuming it
    if (writePos - readPos < 4) {
      dry = true;
      out[i * 2] = out[i * 2 + 1] = 0;
      tap.push(0);
      continue;
    }
    {
      int16_t frame[2];
      memcpy(frame, &ring[readPos % kRingBytes], 4);
      nextL = frame[0];
      nextR = frame[1];
    }

    float l = (prevL + (nextL - prevL) * (float)phase) * (1.0f / 32768);
    float r = (prevR + (nextR - prevR) * (float)phase) * (1.0f / 32768);
    if (useEq) eq.process(l, r);
    tap.push((l + r) * 0.5f);
    l *= vol;
    r *= vol;
    // soft knee above -1 dBFS instead of hard clipping boosted peaks
    auto soft = [](float x) {
      const float k = 0.89f;
      float a = fabsf(x);
      if (a <= k) return x;
      float y = k + (1 - k) * tanhf((a - k) / (1 - k));
      return x < 0 ? -y : y;
    };
    out[i * 2] = (int16_t)(soft(l) * 32767);
    out[i * 2 + 1] = (int16_t)(soft(r) * 32767);

    phase += step;
    while (phase >= 1.0) {
      phase -= 1.0;
      int16_t pl, pr;
      if (!popFrame(pl, pr)) {
        dry = true;
        break;
      }
      prevL = pl;
      prevR = pr;
      played++;
    }
  }

  if (dry && !paused && onUnderrun)
    onUnderrun();
}

void AudioOutput::threadEntry(void* arg) {
  static_cast<AudioOutput*>(arg)->run();
}

void AudioOutput::run() {
  for (int i = 0; i < kNumBuffers; i++) {
    fill((int16_t*)bufferMem[i], kOutFrames);
    audoutAppendAudioOutBuffer(&buffers[i]);
  }

  int errorCount = 0;
  while (running) {
    AudioOutBuffer* released = nullptr;
    u32 count = 0;
    Result rc = audoutWaitPlayFinish(&released, &count, 100'000'000ULL);
    if (R_FAILED(rc) || !released || count == 0) {
      // Never spin: we share core 3 with system processes, and a busy loop
      // here freezes HOME menu and everything else on that core.
      if (R_FAILED(rc) && ++errorCount <= 5)
        LOG("audout wait failed: 0x%x", rc);
      svcSleepThread(10'000'000ULL);
      continue;
    }

    // audout hands back a linked list of finished buffers
    for (AudioOutBuffer* b = released; b && count > 0; count--) {
      AudioOutBuffer* next = b->next;
      fill((int16_t*)b->buffer, kOutFrames);
      b->next = nullptr;
      audoutAppendAudioOutBuffer(b);
      b = next;
    }
  }
}
