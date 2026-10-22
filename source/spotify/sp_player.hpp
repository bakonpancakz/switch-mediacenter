#pragma once

#include <switch.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "sp_audio.hpp"
#include "switchify_ipc.h"

namespace cspot {
class SpircHandler;
}

// Glue between cspot's SpircHandler and the audio output, plus the status
// snapshot the overlay reads. Mirrors cspot's CliPlayer event handling.
class Player {
 public:
  explicit Player(AudioOutput& audio);

  void attach(std::shared_ptr<cspot::SpircHandler> handler);
  void detach();

  // Drives work that must not run on the audio or IPC threads. Called from the
  // session loop, the only thread of ours that calls into cspot.
  void tick();

  // Overlay controls: safe from any thread, applied on the next tick()
  void playPause();
  void next();
  void prev();
  void setVolume(u32 percent);
  // Changes our output only, without telling Spotify (for slider drags)
  void previewVolume(u32 percent);
  void seek(u32 ms);

  void setState(SwitchifyState s, const std::string& message = "");
  void setDeviceInfo(const std::string& name, const std::string& ip);
  void setUsername(const std::string& name);
  void getStatus(SwitchifyStatus& out);
  std::string getImageUrl();

 private:
  AudioOutput& audio;
  std::shared_ptr<cspot::SpircHandler> handler;
  std::mutex handlerMutex;

  std::mutex statusMutex;
  SwitchifyStatus status = {};
  std::string imageUrl;  // guarded by statusMutex

  // Set from the audio thread, consumed in tick()
  std::atomic<bool> trackReached = false;
  std::atomic<bool> underrun = false;
  std::atomic<u64> trackReachedFrames = 0;

  // Queued overlay commands, consumed in tick()
  std::atomic<bool> pendingPlayPause = false;
  std::atomic<bool> pendingNext = false;
  std::atomic<bool> pendingPrev = false;
  std::atomic<s32> pendingVolume = -1;
  std::atomic<s64> pendingSeekMs = -1;
  std::atomic<bool> playlistEnd = false;
  std::atomic<u32> pendingStartMs = 0;

  // Playback position = baseMs + frames played since baseFrames
  std::atomic<u32> baseMs = 0;
  std::atomic<u64> baseFrames = 0;

  void resetPosition(u32 ms);
  u32 currentPositionMs();

  // Set on seek; tick() re-reports the position once audio flows again
  bool seekResyncPending = false;
  u64 seekResyncFrames = 0;

  // What the Spotify app believes: position at a moment, extrapolated with
  // wall time. When real playback drifts (stalls), we correct it.
  u32 noteMs = 0;
  u64 noteAt = 0, lastCorrection = 0;
  bool wasPlaying = false;
  void notePosition(u32 ms);
};
