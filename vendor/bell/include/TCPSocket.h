#ifndef BELL_BASIC_SOCKET_H
#define BELL_BASIC_SOCKET_H

#include <ctype.h>
#include <stdlib.h>
#include <sys/types.h>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include "BellSocket.h"
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include "win32shim.h"
#else
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef __sun
#include <sys/filio.h>
#endif
#endif
#include <BellLogger.h>
#include <fstream>
#include <sstream>

namespace bell {
#ifndef _WIN32
// Switchify: resolve and connect with a time limit. A plain blocking connect
// waits out every SYN retry (over a minute) when the network drops, which
// kept threads alive past exit. Returns the connected fd or -1.
inline int connectWithTimeout(const std::string& host, uint16_t port, int timeoutMs) {
  struct addrinfo hints {}, *addr = nullptr;
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  char portStr[6];
  snprintf(portStr, sizeof(portStr), "%u", port);
  if (getaddrinfo(host.c_str(), portStr, &hints, &addr) != 0 || !addr) return -1;
  int fd = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
  if (fd < 0) { freeaddrinfo(addr); return -1; }
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  int err = ::connect(fd, addr->ai_addr, addr->ai_addrlen);
  freeaddrinfo(addr);
  if (err < 0 && errno == EINPROGRESS) {
    struct pollfd pfd = {fd, POLLOUT, 0};
    err = -1;
    if (::poll(&pfd, 1, timeoutMs) == 1) {
      int soErr = 0;
      socklen_t len = sizeof(soErr);
      if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soErr, &len) == 0 && soErr == 0) err = 0;
    }
  }
  if (err < 0) { ::close(fd); return -1; }
  fcntl(fd, F_SETFL, flags);  // blocking again, reads use SO_RCVTIMEO
  return fd;
}
#endif

class TCPSocket : public bell::Socket {
 private:
  int sockFd;
  bool isClosed = true;

 public:
  TCPSocket(){};
  ~TCPSocket() { close(); };

  int getFd() { return sockFd; }

  void open(const std::string& host, uint16_t port) {
#ifndef _WIN32
    sockFd = connectWithTimeout(host, port, 10000);
    if (sockFd < 0) {
      BELL_LOG(error, "http", "Could not connect to %s", host.c_str());
      throw std::runtime_error("Resolve failed");
    }
#else
    int err;
    int domain = AF_INET;
    int socketType = SOCK_STREAM;
    struct addrinfo hints {
    }, *addr;
    hints.ai_family = domain;
    hints.ai_socktype = socketType;
    hints.ai_protocol = IPPROTO_IP;
    hints.ai_flags = AI_CANONNAME | AI_ALL | AI_ADDRCONFIG;
    char portStr[6];
    sprintf(portStr, "%u", port);
    err = getaddrinfo(host.c_str(), portStr, &hints, &addr);
    if (err != 0) {
      throw std::runtime_error("Resolve failed");
    }
    sockFd = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
    err = connect(sockFd, addr->ai_addr, addr->ai_addrlen);
    freeaddrinfo(addr);
    if (err < 0) {
      close();
      throw std::runtime_error("Resolve failed");
    }
#endif

    int flag = 1;
    setsockopt(sockFd,       /* socket affected */
               IPPROTO_TCP,  /* set option at TCP level */
               TCP_NODELAY,  /* name of option */
               (char*)&flag, /* the cast is historical cruft */
               sizeof(int)); /* length of option value */

#ifndef _WIN32
    // Switchify: a connection that dies silently (console sleep) must not
    // block reads forever; a timeout reads as EOF upstream
    struct timeval tv = {10, 0};
    setsockopt(sockFd, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv, sizeof(tv));
    setsockopt(sockFd, SOL_SOCKET, SO_SNDTIMEO, (char*)&tv, sizeof(tv));
#endif

    isClosed = false;
  }

  size_t read(uint8_t* buf, size_t len) {
    return recv(sockFd, (char*)buf, len, 0);
  }

  size_t write(uint8_t* buf, size_t len) {
    return send(sockFd, (char*)buf, len, 0);
  }

  size_t poll() {
#ifdef _WIN32
    unsigned long value;
    ioctlsocket(sockFd, FIONREAD, &value);
#else
    int value;
    ioctl(sockFd, FIONREAD, &value);
#endif
    return value;
  }
  bool isOpen() {
    return !isClosed;
  }

  void close() {
    if (!isClosed) {
#ifdef _WIN32
      closesocket(sockFd);
#else
      ::close(sockFd);
#endif
      sockFd = -1;
      isClosed = true;
    }
  }
};

}  // namespace bell

#endif
