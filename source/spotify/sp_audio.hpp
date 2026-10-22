#pragma once

#include <switch.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <vector>

#include "sp_dsp.hpp"

// Plays 44.1kHz stereo s16 PCM (what cspot's tremor decoder emits) through
// audout, which only runs at 48kHz, so we resample on the way out.
class AudioOutput {
 public:
  static constexpr u32 kInputRate = 44100;

  // Both fire on the audio thread with the ring lock held: keep them to
  // setting flags, never call back into cspot from here.
  // Playback crossed into a new track
  std::function<void(size_t trackHash)> onTrackReached;
  // The ring ran dry while unpaused
  std::function<void()> onUnderrun;

  bool start();
  void stop();

  // Returns bytes accepted (may be less than len when the ring is full)
  size_t writePCM(const uint8_t* data, size_t len, size_t trackHash);
  void clear();

  void setPaused(bool p) { paused = p; }
  bool isPaused() const { return paused; }
  void setVolume(float v) { volume = v; }

  // Input frames actually sent to the speaker since start
  u64 framesPlayed() const { return played; }

  static constexpr u32 kOutputRate = 48000;
  dsp::Equalizer eq;     // applied to the 48 kHz output
  dsp::SpectrumTap tap;  // what the visualizer sees (after EQ, before volume)
  // Output samples queued in audout ahead of the speaker
  static constexpr int queuedSamples() { return kNumBuffers * (int)kOutFrames; }

 private:
  static constexpr size_t kRingBytes = 128 * 1024;  // ~0.75s of 44.1k stereo
  static constexpr int kNumBuffers = 4;
  static constexpr size_t kOutFrames = 2048;  // per audout buffer (~43ms)
  static constexpr size_t kOutBytes = (kOutFrames * 4 + 0xFFF) & ~0xFFF;

  struct Marker {
    u64 pos;
    size_t hash;
  };

  std::vector<uint8_t> ring = std::vector<uint8_t>(kRingBytes);
  u64 writePos = 0, readPos = 0;  // absolute byte counters
  std::deque<Marker> markers;
  size_t lastHash = 0;     // last track written (reset by clear())
  size_t reachedHash = 0;  // last track that reached the speaker (kept)
  std::mutex ringMutex;

  AudioOutBuffer buffers[kNumBuffers] = {};
  void* bufferMem[kNumBuffers] = {};

  Thread thread;
  std::atomic<bool> running = false;
  std::atomic<bool> paused = true;
  std::atomic<float> volume = 1.0f;
  std::atomic<u64> played = 0;

  // Resampler state: fractional read cursor between prev and next frame
  double phase = 0.0;
  int16_t prevL = 0, prevR = 0;

  static void threadEntry(void* arg);
  void run();
  void fill(int16_t* out, size_t frames);
  bool popFrame(int16_t& l, int16_t& r);
};
