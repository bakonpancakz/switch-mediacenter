#include "sp_player.hpp"

#include <cstdlib>
#include <cstring>
#include <string_view>
#include <variant>

#include "SpircHandler.h"
#include "TrackPlayer.h"
#include "TrackQueue.h"
#include "sp_log.hpp"

namespace {
void copyStr(char* dst, size_t cap, const std::string& src) {
  strncpy(dst, src.c_str(), cap - 1);
  dst[cap - 1] = '\0';
}

// Spotify volume is 0..65535; a squared curve sounds closer to linear
float gainFor(u32 percent) {
  float v = percent / 100.0f;
  return v * v;
}
}  // namespace

Player::Player(AudioOutput& audio) : audio(audio) {
  status.state = SwitchifyState_Starting;
  status.volume = 100;
  audio.setVolume(gainFor(status.volume));

  audio.onTrackReached = [this](size_t) {
    trackReachedFrames = this->audio.framesPlayed();
    trackReached = true;
  };
  audio.onUnderrun = [this]() { underrun = true; };
}

void Player::resetPosition(u32 ms) {
  baseMs = ms;
  baseFrames = audio.framesPlayed();
}

void Player::attach(std::shared_ptr<cspot::SpircHandler> h) {
  std::lock_guard<std::mutex> lock(handlerMutex);
  handler = h;

  auto hashFunc = std::hash<std::string_view>();
  handler->getTrackPlayer()->setDataCallback(
      [this, hashFunc](uint8_t* data, size_t bytes, std::string_view trackId) {
        return audio.writePCM(data, bytes, hashFunc(trackId));
      });

  handler->setEventHandler([this](std::unique_ptr<cspot::SpircHandler::Event> event) {
    using Ev = cspot::SpircHandler::EventType;
    switch (event->eventType) {
      case Ev::PLAY_PAUSE: {
        bool paused = std::get<bool>(event->data);
        audio.setPaused(paused);
        if (paused)
          resetPosition(currentPositionMs());
        else
          baseFrames = audio.framesPlayed();
        setState(paused ? SwitchifyState_Paused : SwitchifyState_Playing);
        break;
      }
      case Ev::FLUSH:
        audio.clear();
        break;
      case Ev::DISC: {
        audio.clear();
        audio.setPaused(true);
        setState(SwitchifyState_Idle, "Another device took over playback");
        break;
      }
      case Ev::SEEK: {
        audio.clear();
        resetPosition((u32)std::get<int>(event->data));
        notePosition((u32)std::get<int>(event->data));
        break;
      }
      case Ev::PLAYBACK_START: {
        audio.setPaused(true);
        audio.clear();
        playlistEnd = false;
        if (auto* pos = std::get_if<int>(&event->data))
          pendingStartMs = (u32)*pos;
        break;
      }
      case Ev::DEPLETED:
        playlistEnd = true;
        break;
      case Ev::VOLUME: {
        u32 pct = (u32)std::get<int>(event->data) * 100 / 65535;
        audio.setVolume(gainFor(pct));
        std::lock_guard<std::mutex> lock(statusMutex);
        status.volume = pct;
        break;
      }
      case Ev::TRACK_INFO: {
        auto info = std::get<cspot::TrackInfo>(event->data);
        LOG("Now playing: %s - %s", info.artist.c_str(), info.name.c_str());
        std::lock_guard<std::mutex> lock(statusMutex);
        copyStr(status.title, sizeof(status.title), info.name);
        copyStr(status.artist, sizeof(status.artist), info.artist);
        copyStr(status.album, sizeof(status.album), info.album);
        status.duration_ms = info.duration;
        imageUrl = info.imageUrl;
        break;
      }
      default:
        break;
    }
  });
}

void Player::detach() {
  std::lock_guard<std::mutex> lock(handlerMutex);
  audio.setPaused(true);
  audio.clear();
  handler = nullptr;
  std::lock_guard<std::mutex> slock(statusMutex);
  status.title[0] = status.artist[0] = status.album[0] = '\0';
  status.duration_ms = status.position_ms = 0;
  imageUrl.clear();
}

std::string Player::getImageUrl() {
  std::lock_guard<std::mutex> lock(statusMutex);
  return imageUrl;
}

void Player::tick() {
  std::lock_guard<std::mutex> lock(handlerMutex);
  if (!handler)
    return;

  if (trackReached.exchange(false)) {
    baseMs = pendingStartMs.exchange(0);
    baseFrames = trackReachedFrames.load();
    handler->notifyAudioReachedPlayback();
    notePosition(currentPositionMs());
  }

  if (underrun.exchange(false) && playlistEnd) {
    playlistEnd = false;
    handler->notifyAudioEnded();
    setState(SwitchifyState_Idle);
  }

  // Overlay commands, queued by the IPC thread. "Loaded" means Spotify has
  // started a track here; the title can lag behind the first audio.
  u32 state;
  {
    std::lock_guard<std::mutex> slock(statusMutex);
    state = status.state;
  }
  bool loaded = state == SwitchifyState_Playing || state == SwitchifyState_Paused;

  if (pendingPlayPause.exchange(false)) {
    if (loaded) {
      LOG("overlay: %s", audio.isPaused() ? "play" : "pause");
      handler->setPause(!audio.isPaused());
    } else {
      LOG("overlay: play/pause ignored, nothing loaded (state %u)", state);
    }
  }
  if (pendingNext.exchange(false)) {
    bool ok = handler->nextSong();
    LOG("overlay: next -> %s", ok ? "ok" : "end of queue");
    if (ok)
      audio.clear();
  }
  if (pendingPrev.exchange(false)) {
    bool ok = handler->previousSong();
    LOG("overlay: prev -> %s", ok ? "ok" : "start of queue");
    if (ok)
      audio.clear();
  }
  s32 vol = pendingVolume.exchange(-1);
  if (vol >= 0)
    handler->setRemoteVolume(vol * 65535 / 100);

  // Same steps cspot takes for a seek frame from the phone, plus telling
  // Spotify the new position so the app's progress bar follows
  s64 seekMs = pendingSeekMs.exchange(-1);
  if (seekMs >= 0) {
    LOG("overlay: seek to %lld ms%s", (long long)seekMs, loaded ? "" : " ignored, nothing loaded");
    if (loaded) {
      // cspot treats a pending seek of 0 as "no seek"
      size_t target = seekMs > 0 ? (size_t)seekMs : 1;
      handler->getTrackPlayer()->seekMs(target);
      audio.clear();
      resetPosition((u32)seekMs);
      handler->updatePositionMs((u32)seekMs);
      notePosition((u32)seekMs);
      seekResyncFrames = audio.framesPlayed();
      seekResyncPending = true;
    }
  }

  // The decoder needs a few range requests to land on the new position, so
  // tell Spotify again once audio is actually flowing; otherwise the app's
  // progress bar runs ahead of what we're playing.
  if (seekResyncPending && audio.framesPlayed() > seekResyncFrames + AudioOutput::kInputRate / 5) {
    seekResyncPending = false;
    handler->updatePositionMs(currentPositionMs());
    notePosition(currentPositionMs());
  }

  // Keep the phone's progress bar honest after stalls (network hiccups,
  // buffer underruns), without flooding Spotify with updates.
  bool playingNow = state == SwitchifyState_Playing && !audio.isPaused();
  u64 now = armTicksToNs(armGetSystemTick()) / 1000000ULL;
  if (playingNow && !wasPlaying) notePosition(currentPositionMs());
  wasPlaying = playingNow;
  if (playingNow && noteAt) {
    s64 expected = (s64)noteMs + (s64)(now - noteAt);
    s64 actual = currentPositionMs();
    if (llabs(actual - expected) > 2000 && now - lastCorrection > 5000) {
      LOG("position drifted %lld ms, resyncing", (long long)(actual - expected));
      handler->updatePositionMs((u32)actual);
      notePosition((u32)actual);
      lastCorrection = now;
    }
  }
}

void Player::notePosition(u32 ms) {
  noteMs = ms;
  noteAt = armTicksToNs(armGetSystemTick()) / 1000000ULL;
}

u32 Player::currentPositionMs() {
  return baseMs + (u32)((audio.framesPlayed() - baseFrames) * 1000 / AudioOutput::kInputRate);
}

// These run on the IPC thread: only queue work for tick(), never touch cspot
// here (small stack, and cspot isn't safe to call from several threads).
void Player::playPause() {
  pendingPlayPause = true;
}

void Player::next() {
  pendingNext = true;
}

void Player::prev() {
  pendingPrev = true;
}

void Player::setVolume(u32 percent) {
  if (percent > 100)
    percent = 100;
  audio.setVolume(gainFor(percent));
  {
    std::lock_guard<std::mutex> lock(statusMutex);
    status.volume = percent;
  }
  pendingVolume = (s32)percent;
}

void Player::previewVolume(u32 percent) {
  audio.setVolume(gainFor(percent > 100 ? 100 : percent));
}

void Player::seek(u32 ms) {
  pendingSeekMs = ms;
}
void Player::setState(SwitchifyState s, const std::string& message) {
  std::lock_guard<std::mutex> lock(statusMutex);
  status.state = s;
  copyStr(status.message, sizeof(status.message), message);
}

void Player::setDeviceInfo(const std::string& name, const std::string& ip) {
  std::lock_guard<std::mutex> lock(statusMutex);
  copyStr(status.device_name, sizeof(status.device_name), name);
  copyStr(status.ip, sizeof(status.ip), ip);
}

void Player::setUsername(const std::string& name) {
  std::lock_guard<std::mutex> lock(statusMutex);
  copyStr(status.username, sizeof(status.username), name);
}

void Player::getStatus(SwitchifyStatus& out) {
  std::lock_guard<std::mutex> lock(statusMutex);
  out = status;
  if (status.state == SwitchifyState_Playing) {
    out.position_ms = currentPositionMs();
  } else {
    out.position_ms = baseMs;
  }
  if (out.duration_ms && out.position_ms > out.duration_ms)
    out.position_ms = out.duration_ms;
}
