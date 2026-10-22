#include "sp_zeroconf.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "LoginBlob.h"
#include "sp_log.hpp"

namespace {

constexpr u16 kMdnsPort = 5353;
constexpr const char* kMdnsGroup = "224.0.0.251";
constexpr const char* kServiceType = "_spotify-connect._tcp.local";
constexpr const char* kServicesMeta = "_services._dns-sd._udp.local";

// ---- small helpers --------------------------------------------------------

std::string urlDecode(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); i++) {
    char c = in[i];
    if (c == '+') {
      out += ' ';
    } else if (c == '%' && i + 2 < in.size() && isxdigit((unsigned char)in[i + 1]) &&
               isxdigit((unsigned char)in[i + 2])) {
      out += (char)strtol(in.substr(i + 1, 2).c_str(), nullptr, 16);
      i += 2;
    } else {
      out += c;
    }
  }
  return out;
}

std::map<std::string, std::string> parseForm(const std::string& body) {
  std::map<std::string, std::string> out;
  size_t start = 0;
  while (start <= body.size()) {
    size_t amp = body.find('&', start);
    if (amp == std::string::npos)
      amp = body.size();
    std::string pair = body.substr(start, amp - start);
    size_t eq = pair.find('=');
    if (eq != std::string::npos)
      out[urlDecode(pair.substr(0, eq))] = urlDecode(pair.substr(eq + 1));
    start = amp + 1;
  }
  return out;
}

bool sendAll(int fd, const std::string& data) {
  size_t sent = 0;
  while (sent < data.size()) {
    ssize_t n = send(fd, data.data() + sent, data.size() - sent, 0);
    if (n <= 0)
      return false;
    sent += n;
  }
  return true;
}

bool sendJson(int fd, const std::string& json) {
  std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                     "Connection: close\r\nContent-Length: " +
                     std::to_string(json.size()) + "\r\n\r\n" + json;
  bool ok = sendAll(fd, resp);
  if (!ok)
    LOG("zeroconf: sending reply failed (errno %d)", errno);
  return ok;
}

// ---- DNS wire format --------------------------------------------------------

struct DnsWriter {
  uint8_t* buf;
  size_t cap;
  size_t len = 0;
  bool overflow = false;

  void u8(uint8_t v) {
    if (len + 1 > cap) {
      overflow = true;
      return;
    }
    buf[len++] = v;
  }
  void u16(uint16_t v) {
    u8(v >> 8);
    u8(v & 0xFF);
  }
  void u32(uint32_t v) {
    u16(v >> 16);
    u16(v & 0xFFFF);
  }
  void bytes(const void* p, size_t n) {
    for (size_t i = 0; i < n; i++)
      u8(((const uint8_t*)p)[i]);
  }
  // Dotted name -> labels. Instance names may contain spaces, which is fine.
  // The first label of an instance name is taken whole (it may contain dots).
  void name(const std::string& n, size_t firstLabelLen = 0) {
    size_t pos = 0;
    if (firstLabelLen) {
      u8((uint8_t)firstLabelLen);
      bytes(n.data(), firstLabelLen);
      pos = firstLabelLen + 1;
    }
    while (pos < n.size()) {
      size_t dot = n.find('.', pos);
      if (dot == std::string::npos)
        dot = n.size();
      u8((uint8_t)(dot - pos));
      bytes(n.data() + pos, dot - pos);
      pos = dot + 1;
    }
    u8(0);
  }
  // Writes a 16-bit length placeholder, returns its offset
  size_t beginRdata() {
    size_t at = len;
    u16(0);
    return at;
  }
  void endRdata(size_t at) {
    if (overflow)
      return;
    uint16_t n = (uint16_t)(len - at - 2);
    buf[at] = n >> 8;
    buf[at + 1] = n & 0xFF;
  }
};

// Reads a (possibly compressed) name from a DNS packet, lowercased
bool readName(const uint8_t* pkt, size_t pktLen, size_t& off, std::string& out) {
  size_t pos = off;
  bool jumped = false;
  int hops = 0;
  out.clear();
  while (pos < pktLen) {
    uint8_t l = pkt[pos];
    if (l == 0) {
      if (!jumped)
        off = pos + 1;
      return true;
    }
    if ((l & 0xC0) == 0xC0) {
      if (pos + 1 >= pktLen || ++hops > 16)
        return false;
      if (!jumped)
        off = pos + 2;
      pos = ((l & 0x3F) << 8) | pkt[pos + 1];
      jumped = true;
      continue;
    }
    if (pos + 1 + l > pktLen)
      return false;
    if (!out.empty())
      out += '.';
    for (size_t i = 0; i < l; i++)
      out += (char)tolower(pkt[pos + 1 + i]);
    pos += 1 + l;
  }
  return false;
}

std::string lowerStr(std::string s) {
  for (auto& c : s)
    c = (char)tolower((unsigned char)c);
  return s;
}

}  // namespace

bool Zeroconf::openSockets() {
  // HTTP listener
  httpFd = socket(AF_INET, SOCK_STREAM, 0);
  if (httpFd < 0) {
    LOG("zeroconf: http socket failed");
    return false;
  }
  int yes = 1;
  setsockopt(httpFd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(kHttpPort);
  addr.sin_addr.s_addr = INADDR_ANY;
  if (bind(httpFd, (sockaddr*)&addr, sizeof(addr)) < 0 || listen(httpFd, 4) < 0) {
    LOG("zeroconf: http bind/listen failed");
    return false;
  }

  // mDNS responder
  mdnsFd = socket(AF_INET, SOCK_DGRAM, 0);
  if (mdnsFd < 0) {
    LOG("zeroconf: mdns socket failed");
    return false;
  }
  setsockopt(mdnsFd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  addr.sin_port = htons(kMdnsPort);
  if (bind(mdnsFd, (sockaddr*)&addr, sizeof(addr)) < 0) {
    LOG("zeroconf: mdns bind failed");
    return false;
  }
  ip_mreq mreq = {};
  mreq.imr_multiaddr.s_addr = inet_addr(kMdnsGroup);
  mreq.imr_interface.s_addr = ip;
  if (setsockopt(mdnsFd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0)
    LOG("zeroconf: IP_ADD_MEMBERSHIP failed, discovery may not work");
  u8 ttl = 255;
  setsockopt(mdnsFd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
  return true;
}

void Zeroconf::closeSockets() {
  if (httpFd >= 0)
    close(httpFd);
  if (mdnsFd >= 0)
    close(mdnsFd);
  httpFd = mdnsFd = -1;
}

size_t Zeroconf::buildResponse(uint8_t* buf, size_t cap) {
  DnsWriter w{buf, cap};
  std::string instance = instanceName + "." + kServiceType;
  size_t instLabel = instanceName.size();

  w.u16(0);       // id
  w.u16(0x8400);  // response, authoritative
  w.u16(0);       // questions
  w.u16(4);       // answers
  w.u16(0);
  w.u16(0);

  // PTR _spotify-connect._tcp.local -> instance
  w.name(kServiceType);
  w.u16(12);
  w.u16(1);
  w.u32(4500);
  size_t at = w.beginRdata();
  w.name(instance, instLabel);
  w.endRdata(at);

  // SRV instance -> host:port  (cache-flush bit set on unique records)
  w.name(instance, instLabel);
  w.u16(33);
  w.u16(0x8001);
  w.u32(120);
  at = w.beginRdata();
  w.u16(0);
  w.u16(0);
  w.u16(kHttpPort);
  w.name(hostName);
  w.endRdata(at);

  // TXT instance
  w.name(instance, instLabel);
  w.u16(16);
  w.u16(0x8001);
  w.u32(4500);
  at = w.beginRdata();
  for (const char* kv : {"VERSION=1.0", "CPath=/spotify_info", "Stack=SP"}) {
    w.u8((uint8_t)strlen(kv));
    w.bytes(kv, strlen(kv));
  }
  w.endRdata(at);

  // A host -> ip
  w.name(hostName);
  w.u16(1);
  w.u16(0x8001);
  w.u32(120);
  at = w.beginRdata();
  w.bytes(&ip, 4);  // already network order
  w.endRdata(at);

  return w.overflow ? 0 : w.len;
}

void Zeroconf::sendAnnouncement(const sockaddr_in* to) {
  uint8_t buf[1024];
  size_t len = buildResponse(buf, sizeof(buf));
  if (!len)
    return;
  sockaddr_in group = {};
  group.sin_family = AF_INET;
  group.sin_port = htons(kMdnsPort);
  group.sin_addr.s_addr = inet_addr(kMdnsGroup);
  if (!to)
    to = &group;
  sendto(mdnsFd, buf, len, 0, (const sockaddr*)to, sizeof(*to));
}

void Zeroconf::handleMdnsPacket() {
  uint8_t pkt[1500];
  sockaddr_in from = {};
  socklen_t fromLen = sizeof(from);
  ssize_t n = recvfrom(mdnsFd, pkt, sizeof(pkt), 0, (sockaddr*)&from, &fromLen);
  if (n < 12)
    return;
  if (pkt[2] & 0x80)  // ignore responses
    return;

  u16 qdcount = (pkt[4] << 8) | pkt[5];
  std::string instance = lowerStr(instanceName + "." + kServiceType);
  std::string host = lowerStr(hostName);
  size_t off = 12;
  bool match = false;
  bool wantsMeta = false;

  for (u16 q = 0; q < qdcount && off < (size_t)n; q++) {
    std::string qname;
    if (!readName(pkt, n, off, qname) || off + 4 > (size_t)n)
      return;
    off += 4;  // qtype + qclass
    if (qname == kServiceType || qname == instance || qname == host)
      match = true;
    if (qname == kServicesMeta)
      wantsMeta = true;
  }

  // Legacy unicast queries (source port != 5353) expect a direct reply
  bool unicast = ntohs(from.sin_port) != kMdnsPort;

  if (match)
    sendAnnouncement(unicast ? &from : nullptr);

  if (wantsMeta) {
    uint8_t buf[256];
    DnsWriter w{buf, sizeof(buf)};
    w.u16(0);
    w.u16(0x8400);
    w.u16(0);
    w.u16(1);
    w.u16(0);
    w.u16(0);
    w.name(kServicesMeta);
    w.u16(12);
    w.u16(1);
    w.u32(4500);
    size_t at = w.beginRdata();
    w.name(kServiceType);
    w.endRdata(at);
    if (!w.overflow) {
      sockaddr_in group = {};
      group.sin_family = AF_INET;
      group.sin_port = htons(kMdnsPort);
      group.sin_addr.s_addr = inet_addr(kMdnsGroup);
      const sockaddr_in* to = unicast ? &from : &group;
      sendto(mdnsFd, buf, w.len, 0, (const sockaddr*)to, sizeof(*to));
    }
  }
}

bool Zeroconf::handleHttpClient(int fd, std::shared_ptr<cspot::LoginBlob>& blob) {
  std::string req;
  char buf[2048];
  size_t headerEnd = std::string::npos;
  size_t contentLength = 0;

  // Read headers (and whatever body arrives with them)
  while (headerEnd == std::string::npos && req.size() < 16384) {
    pollfd p = {fd, POLLIN, 0};
    if (poll(&p, 1, 3000) <= 0)
      return false;
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0)
      return false;
    req.append(buf, n);
    headerEnd = req.find("\r\n\r\n");
  }
  if (headerEnd == std::string::npos)
    return false;

  std::string headers = lowerStr(req.substr(0, headerEnd));
  size_t cl = headers.find("content-length:");
  if (cl != std::string::npos)
    contentLength = strtoul(headers.c_str() + cl + 15, nullptr, 10);

  std::string body = req.substr(headerEnd + 4);
  while (body.size() < contentLength && body.size() < 65536) {
    pollfd p = {fd, POLLIN, 0};
    if (poll(&p, 1, 3000) <= 0)
      break;
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0)
      break;
    body.append(buf, n);
  }

  std::string requestLine = req.substr(0, req.find("\r\n"));
  LOG("zeroconf: %s", requestLine.c_str());

  if (requestLine.rfind("GET ", 0) == 0) {
    std::string info = blob->buildZeroconfInfo();
    LOG("zeroconf: getInfo reply (%u bytes): %s", (unsigned)info.size(), info.c_str());
    if (sendJson(fd, info))
      LOG("zeroconf: getInfo sent, waiting for the app to send addUser");
    return false;
  }

  if (requestLine.rfind("POST ", 0) == 0) {
    auto form = parseForm(body);
    std::string action = form["action"];
    LOG("zeroconf: POST action='%s' body=%u/%u bytes", action.c_str(), (unsigned)body.size(),
        (unsigned)contentLength);
    if (action != "addUser") {
      sendJson(fd, "{\"status\":101,\"spotifyError\":0,\"statusString\":\"ERROR-OK\"}");
      return false;
    }
    bool ok = false;
    try {
      blob->loadZeroconfQuery(form);
      ok = !blob->authData.empty();
    } catch (const std::exception& e) {
      LOG("zeroconf: blob decode failed: %s", e.what());
    }
    if (ok) {
      sendJson(fd, "{\"status\":101,\"spotifyError\":0,\"statusString\":\"ERROR-OK\"}");
    } else {
      sendJson(fd, "{\"status\":202,\"spotifyError\":-1,\"statusString\":\"ERROR-LOGIN-FAILED\"}");
    }
    LOG("zeroconf: addUser from '%s' -> %s", form["userName"].c_str(), ok ? "ok" : "failed");
    return ok;
  }

  sendAll(fd, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
  return false;
}

bool Zeroconf::waitForLogin(std::shared_ptr<cspot::LoginBlob> blob, u32 ipv4,
                            const std::atomic<bool>& cancel) {
  ip = ipv4;
  instanceName = blob->getDeviceName();
  char hostBuf[32];
  snprintf(hostBuf, sizeof(hostBuf), "switchify-%08x.local", (unsigned)ipv4);
  hostName = hostBuf;

  if (!openSockets()) {
    closeSockets();
    return false;
  }
  LOG("zeroconf: advertising '%s' on port %u", instanceName.c_str(), kHttpPort);

  // Initial burst of announcements, then periodic re-announce
  u64 nextAnnounce = 0;
  int burst = 3;
  bool loggedIn = false;

  while (!cancel && !loggedIn) {
    u64 now = armTicksToNs(armGetSystemTick()) / 1'000'000ULL;
    if (now >= nextAnnounce) {
      sendAnnouncement(nullptr);
      nextAnnounce = now + (burst > 0 ? 1000 : 60000);
      if (burst > 0)
        burst--;
    }

    pollfd fds[2] = {{httpFd, POLLIN, 0}, {mdnsFd, POLLIN, 0}};
    if (poll(fds, 2, 500) <= 0)
      continue;

    if (fds[1].revents & POLLIN)
      handleMdnsPacket();

    if (fds[0].revents & POLLIN) {
      int client = accept(httpFd, nullptr, nullptr);
      if (client >= 0) {
        loggedIn = handleHttpClient(client, blob);
        close(client);
      }
    }
  }

  closeSockets();
  return loggedIn;
}
