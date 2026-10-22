#include "PlainConnection.h"

#ifndef _WIN32
#include <arpa/inet.h>  // for htonl, ntohl
#include <netdb.h>      // for addrinfo, freeaddrinfo, getaddrinfo
#include <netinet/in.h>   // for IPPROTO_IP, IPPROTO_TCP
#include <netinet/tcp.h>  // for TCP_NODELAY
#include <sys/errno.h>    // for EAGAIN, EINTR, ETIMEDOUT, errno
#include <sys/socket.h>   // for setsockopt, connect, recv, send, shutdown
#include <sys/time.h>     // for timeval
#include <cstring>        // for memset
#include <stdexcept>      // for runtime_error
#else
#include <ws2tcpip.h>
#endif
#include "BellLogger.h"  // for AbstractLogger
#include "BellUtils.h"   // for BELL_SLEEP_MS
#include "TCPSocket.h"   // for connectWithTimeout
#include "Logger.h"      // for CSPOT_LOG
#include "Packet.h"      // for cspot
#include "Utils.h"       // for extract, pack

using namespace cspot;

static int getErrno() {
#ifdef _WIN32
  int code = WSAGetLastError();
  if (code == WSAETIMEDOUT)
    return ETIMEDOUT;
  if (code == WSAEINTR)
    return EINTR;
  return code;
#else
  return errno;
#endif
}

PlainConnection::PlainConnection() {
  this->apSock = -1;
};

PlainConnection::~PlainConnection() {
  this->close();
};

void PlainConnection::connect(const std::string& apAddress) {
  std::string hostname = apAddress.substr(0, apAddress.find(":"));
  std::string portStr =
      apAddress.substr(apAddress.find(":") + 1, apAddress.size());

  // Switchify: connect with a time limit. The old loop waited out every SYN
  // retry when the network dropped, and read an unset list if DNS failed.
  this->apSock = bell::connectWithTimeout(hostname, (uint16_t)atoi(portStr.c_str()), 10000);
  if (this->apSock < 0)
    throw std::runtime_error("Can't connect to spotify servers");
  struct timeval tv;
  tv.tv_sec = 3;
  tv.tv_usec = 0;
  setsockopt(this->apSock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
  setsockopt(this->apSock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof tv);
  int flag = 1;
  setsockopt(this->apSock, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(int));
  CSPOT_LOG(debug, "Connected to spotify server");
}

std::vector<uint8_t> PlainConnection::recvPacket() {
  // Read packet size
  std::vector<uint8_t> packetBuffer(4);
  readBlock(packetBuffer.data(), 4);
  uint32_t packetSize = ntohl(extract<uint32_t>(packetBuffer, 0));

  packetBuffer.resize(packetSize, 0);

  // Read actual data
  readBlock(packetBuffer.data() + 4, packetSize - 4);

  return packetBuffer;
}

std::vector<uint8_t> PlainConnection::sendPrefixPacket(
    const std::vector<uint8_t>& prefix, const std::vector<uint8_t>& data) {
  // Calculate full packet length
  uint32_t actualSize = prefix.size() + data.size() + sizeof(uint32_t);

  // Packet structure [PREFIX] + [SIZE] +  [DATA]
  auto sizeRaw = pack<uint32_t>(htonl(actualSize));
  sizeRaw.insert(sizeRaw.begin(), prefix.begin(), prefix.end());
  sizeRaw.insert(sizeRaw.end(), data.begin(), data.end());

  // Actually write it to the server
  writeBlock(sizeRaw);

  return sizeRaw;
}

void PlainConnection::readBlock(const uint8_t* dst, size_t size) {
  unsigned int idx = 0;
  ssize_t n;
  int retries = 0;

  // Switchify: recv() == 0 means the server closed the connection (e.g. after
  // console sleep). The original code retried that forever, spinning a core
  // at 100%; and on EINTR it added n (-1) to idx.
  while (idx < size) {
    n = recv(this->apSock, (char*)&dst[idx], size - idx, 0);
    if (n == 0)
      throw std::runtime_error("Connection closed by server");
    if (n < 0) {
      switch (getErrno()) {
        case EAGAIN:
        case ETIMEDOUT:
          if (timeoutHandler()) {
            CSPOT_LOG(error, "Connection lost, will need to reconnect...");
            throw std::runtime_error("Reconnection required");
          }
          continue;
        case EINTR:
          continue;
        default:
          if (retries++ > 4)
            throw std::runtime_error("Error in read");
          BELL_SLEEP_MS(100);
          continue;
      }
    }
    idx += n;
  }
}

size_t PlainConnection::writeBlock(const std::vector<uint8_t>& data) {
  unsigned int idx = 0;
  ssize_t n;

  int retries = 0;

  // Switchify: same fixes as readBlock (no spinning, no idx += -1)
  while (idx < data.size()) {
    n = send(this->apSock, (char*)&data[idx],
             data.size() - idx < 64 ? data.size() - idx : 64, 0);
    if (n <= 0) {
      switch (n < 0 ? getErrno() : 0) {
        case EAGAIN:
        case ETIMEDOUT:
          if (timeoutHandler()) {
            throw std::runtime_error("Reconnection required");
          }
          continue;
        case EINTR:
          continue;
        default:
          if (retries++ > 4)
            throw std::runtime_error("Error in write");
          BELL_SLEEP_MS(100);
          continue;
      }
    }
    idx += n;
  }

  return data.size();
}

void PlainConnection::close() {
  if (this->apSock < 0)
    return;

  CSPOT_LOG(info, "Closing socket...");
  shutdown(this->apSock, SHUT_RDWR);
#ifdef _WIN32
  closesocket(this->apSock);
#else
  ::close(this->apSock);
#endif
  this->apSock = -1;
}
