#pragma once

#include <switch.h>

#include <atomic>
#include <memory>
#include <string>

namespace cspot {
class LoginBlob;
}

// Spotify Connect zeroconf pairing: advertises _spotify-connect._tcp over mDNS
// and serves /spotify_info so the phone app can hand us an encrypted login blob.
// Only runs until we've got reusable credentials saved.
class Zeroconf {
 public:
  static constexpr u16 kHttpPort = 7864;

  // Blocks until the phone sends credentials (true) or `cancel` is set (false)
  bool waitForLogin(std::shared_ptr<cspot::LoginBlob> blob, u32 ipv4,
                    const std::atomic<bool>& cancel);

 private:
  int httpFd = -1;
  int mdnsFd = -1;
  u32 ip = 0;
  std::string instanceName;
  std::string hostName;

  bool openSockets();
  void closeSockets();

  // HTTP
  bool handleHttpClient(int fd, std::shared_ptr<cspot::LoginBlob>& blob);

  // mDNS
  void handleMdnsPacket();
  void sendAnnouncement(const struct sockaddr_in* to);
  size_t buildResponse(uint8_t* buf, size_t cap);
};
