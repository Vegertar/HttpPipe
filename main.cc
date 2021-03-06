// main.cc
// Copyright 2014 <Vegertar, vegertar@gmail.com>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <getopt.h>
#ifndef __ANDROID__
#include <ifaddrs.h>
#endif
#include <netdb.h>
#include <net/if.h>
#ifdef __APPLE__
#include <net/if_dl.h>
#endif
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>
#include "pipe.h"

#define VERBOSE(field, ...) do { \
  if (enable_verbose) \
    printf("- \033[32m" #field "\033[0m: " __VA_ARGS__); \
} while (0)

namespace {

const char *program = "pipe";
const char *version = "0.0.1";

bool quit_program;
bool enable_verbose;
bool short_transaction;
char destination[1024];                // destination URL
size_t buffer_size = 1024 * 1024;      // 1 MB
size_t transfer_rate = 12500;          // 100 Kbps
size_t connect_retry = 3;              // 3 times
size_t idle_transfer_interval = 300;   // 5 minutes
size_t idle_transfer_idle_limit = 1;   // 1 time
size_t idle_transfer_busy_limit = 3;   // 3 times
size_t zip_level = 0;                  // disable

inline void Usage();
inline void Version();
const char * GetMacAddress();
size_t ParseSize(const char *s);
size_t ParseRate(const char *s);
size_t ParseInterval(const char *s);
void ParseOptions(int argc, char *argv[]);
void SignalHandler(int signo);

class PostHeader : public v::Header {
 public:
  PostHeader()
      : mac_(NULL),
        path_(NULL),
        compressed_(false),
        persistent_(true),
        host_(),
        buffer_(),
        content_length_offset_(0) {
    // empty
  }

  void SetRequest(const char *method, const char *uri, const char *ver) {
    if (!path_)
      path_ = uri;
    else
      assert(false);
  }

  void SetField(const char *field, const char *value) {
    if (strcasecmp(field, "Host") == 0) {
      snprintf(host_, sizeof(host_), "%s", value);
    } else if (strcasecmp(field, "LETV-TV-MAC") == 0 && !mac_) {
      mac_ = value;
    } else if (strcasecmp(field, "LETV-ZIP") == 0) {
      bool t = value != NULL;
      if (compressed_ != t) {
        content_length_offset_ = 0;
        compressed_ = t;
      }
    } else if (strcasecmp(field, "Connection") == 0) {
      bool t = strcasecmp(value, "close");  // i.e. keep-alive
      if (persistent_ != t) {
        content_length_offset_ = 0;
        persistent_ = t;
      }
    } else {
      assert(false);
    }
  }

  const char * Generate(size_t body_size, size_t *head_size) {
    if (!content_length_offset_) {
      const char *pattern = "POST %s HTTP/1.1\r\n"
                            "Host: %s\r\n"
                            "User-Agent: %s/%s\r\n"
                            "Accept: */*\r\n"
                            "LETV-TV-MAC: %s\r\n"
                            "%s"                  // LETV-ZIP: 1\r\n
                            "%s"                  // Connection: close\r\n
                            "Content-Length: ";
      content_length_offset_ = snprintf(buffer_, sizeof(buffer_),
                                        pattern,
                                        path_,
                                        host_,
                                        program, version,
                                        mac_,
                                        compressed_ ? "LETV-ZIP: 1\r\n" : "",
                                        persistent_ ? "" : "Connection: close\r\n");
    }

    int i = snprintf(buffer_ + content_length_offset_,
                     sizeof(buffer_) - content_length_offset_,
                     "%zu\r\n\r\n", body_size);
    if (head_size)
      *head_size = content_length_offset_ + i;

    return buffer_;
  }

 private:
  const char *mac_;
  const char *path_;
  bool compressed_;
  bool persistent_;
  char host_[64];
  char buffer_[2048];
  int content_length_offset_;
};

}  // anonymous namespace

int main(int argc, char *argv[]) {
  signal(SIGPIPE, SIG_IGN);     // for EPIPE
  signal(SIGINT, SignalHandler);
  signal(SIGTERM, SignalHandler);
  signal(SIGQUIT, SignalHandler);

  ParseOptions(argc, argv);

  PostHeader header;
  header.SetField("LETV-TV-MAC", GetMacAddress());
  if (short_transaction)
    header.SetField("Connection", "close");

  v::HttpPipe pipe;
  pipe.Init(STDIN_FILENO, destination);
  pipe.SetBufferSize(buffer_size);
  pipe.SetConnectRetry(connect_retry);
  pipe.SetIdleTransfer(idle_transfer_idle_limit);
  pipe.SetBusyTransfer(idle_transfer_busy_limit);
  pipe.SetTransferRate(transfer_rate);
  pipe.SetZipLevel(zip_level);
  pipe.SetVerbose(enable_verbose);
  pipe.SetHeader(&header);

  pipe.SetStopFlag(&quit_program);
  pipe.Serve(idle_transfer_interval);
}

namespace {

inline void Usage() {
  printf("Usage: %s [options]\n"
         "Pipe standard input to a specific network.\n"
         "\n"
         "Options:\n"
         "  -V             Enable verbose output\n"
         "  -h             Print this help and exit\n"
         "  -v             Print program version and exit\n"
         "  -S             Use short connection\n"
         "  -d DEST        Pipe destination URL\n"
         "  -c LEVEL       Enable ZIP compress (1~9)\n"
         "  -s BUFSIZ      The buffer size, default 1 MB\n"
         "  -r RATE        Transfer rate, default 100 K/s\n"
         "  -n TRY         Failed connect try, default 3 times\n"
         "  -i INTERVAL    Transfer interval, default 5 minutes\n"
         "  -l LIMIT       Limit to transfer occur in idle, default 1 times\n"
         "  -L LIMIT       Limit to transfer occur in busy, default 3 times\n",
         program);
  exit(0);
}

inline void Version() {
  printf("%s\n", version);
  exit(0);
}

const char * GetMacAddress() {
  static char mac_address[128] = {0};
  if (!mac_address[0]) {
#if defined _GNU_SOURCE
    char buf[1024];
    struct ifreq ifr;
    struct ifconf ifc;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == -1)
      err(1, "%s: socket() error", __func__);

    ifc.ifc_len = sizeof(buf);
    ifc.ifc_buf = buf;
    if (ioctl(sock, SIOCGIFCONF, &ifc) == -1)
      err(1, "%s: ioctl(SIOCGIFCONF) error", __func__);

    struct ifreq *it = ifc.ifc_req;
    struct ifreq *end = it + (ifc.ifc_len / sizeof(struct ifreq));
    for (; it != end; ++it) {
      snprintf(ifr.ifr_name, IFNAMSIZ, "%s", it->ifr_name);
      if (ioctl(sock, SIOCGIFFLAGS, &ifr) == 0) {
        if (!(ifr.ifr_flags & IFF_LOOPBACK)) {
          if (ioctl(sock, SIOCGIFHWADDR, &ifr) == 0) {
            char *to = mac_address;
            unsigned char *from = (unsigned char *)ifr.ifr_hwaddr.sa_data;
            for (int i = 0, n = 0; i < 6; ++i)
              n += snprintf(to + n, sizeof(mac_address) - n, "%02x", from[i]);
            break;
          }
        }
      } else {
        err(1, "%s: ioctl(SIOCGIFFLAGS) error", __func__);
      }
    }
    close(sock);
#elif defined __APPLE__
    struct ifaddrs *ifap, *ifaptr;
    if (getifaddrs(&ifap) == 0) {
      for (ifaptr = ifap; ifaptr != NULL; ifaptr = ifaptr->ifa_next) {
        if ((ifaptr->ifa_addr)->sa_family == AF_LINK &&
            (ifaptr->ifa_flags & IFF_BROADCAST)) {
          unsigned char *from = (unsigned char *)LLADDR((struct sockaddr_dl *)
                                                        ifaptr->ifa_addr);
          char *to = mac_address;
          for (int i = 0, n = 0; i < 6; ++i)
            n += snprintf(to + n, sizeof(mac_address) - n, "%02x", from[i]);
          break;
        }
      }
      freeifaddrs(ifap);
    } else {
      err(1, "%s: getifaddrs error", __func__);
    }
#endif
  }

  VERBOSE(MAC-Address, "%s\n", mac_address);
  return mac_address;
}

size_t ParseSize(const char *s) {
  errno = 0;

  char *endptr;
  size_t value = strtoul(s, &endptr, 10);

  if (errno)
    err(1, "Invalid argument: %s", s);

  switch (*endptr) {
    case 0:  // ok
      break;
    case 'k':
    case 'K':
      value *= 1024;
      break;
    case 'm':
    case 'M':
      value *= 1024 * 1024;
      break;
    default:
      errx(1, "Invalid argument: %s, [0-9]+[kKmM] expect.", s);
  }

  return value;
}

size_t ParseRate(const char *s) {
  errno = 0;

  char *endptr;
  size_t value = strtoul(s, &endptr, 10);

  if (errno)
    err(1, "Invalid argument: %s", s);

  switch (*endptr) {
    case 0:  // ok
      break;
    case 'k':
    case 'K':
      value *= 1000;
      break;
    case 'm':
    case 'M':
      value *= 1000000;
      break;
    default:
      errx(1, "Invalid argument: %s, [0-9]+[kKmM] expect.", s);
  }

  return value / 8;
}

size_t ParseInterval(const char *s) {
  errno = 0;

  char *endptr;
  size_t value = strtoul(s, &endptr, 10);

  if (errno)
    err(1, "Invalid argument: %s", s);

  switch (*endptr) {
    case 0:  // ok
    case 's':
    case 'S':
      break;
    case 'm':
    case 'M':
      value *= 60;
      break;
    case 'h':
    case 'H':
      value *= 3600;
      break;
    default:
      errx(1, "Invalid argument: %s, [0-9]+[sSmMhH] expect.", s);
  }

  return value;
}

void ParseOptions(int argc, char *argv[]) {
  int opt;
  while ((opt = getopt(argc, argv, "VhvSd:c:s:r:n:i:l:L:")) != -1) {
    switch (opt) {
      case 'V':
        enable_verbose = true;
        break;

      case 'h':
        Usage();
        break;

      case 'v':
        Version();
        break;

      case 'S':
        short_transaction = true;
        break;

      case 'd':
        strncpy(destination, optarg, sizeof(destination) - 1);
        break;

      case 'c':
        zip_level = atoi(optarg);
        break;

      case 's':
        buffer_size = ParseSize(optarg);
        break;

      case 'r':
        transfer_rate = ParseRate(optarg);
        break;

      case 'n':
        connect_retry = atoi(optarg);
        break;

      case 'i':
        idle_transfer_interval = ParseInterval(optarg);
        break;

      case 'l':
        idle_transfer_idle_limit = atoi(optarg);
        break;

      case 'L':
        idle_transfer_busy_limit = atoi(optarg);
        break;

      default:
        exit(1);
    }
  }

  if (!destination[0])
    errx(1, "missing destination, expect an URL");

  VERBOSE(Short-Transaction, "%d\n", short_transaction);
  VERBOSE(Zip-Level, "%zu\n", zip_level);
  VERBOSE(Destination, "%s\n", destination);
  VERBOSE(Buffer-Size, "%zu(bytes)\n", buffer_size);
  VERBOSE(Transfer-Rate, "%zu(bytes/s)\n", transfer_rate);
  VERBOSE(Connect-Retry, "%zu(times)\n", connect_retry);
  VERBOSE(Idle-Transfer-Interval, "%zu(sec)\n", idle_transfer_interval);
  VERBOSE(Idle-Transfer-Idle-Limit, "%zu(times)\n", idle_transfer_idle_limit);
  VERBOSE(Idle-Transfer-Busy-Limit, "%zu(times)\n", idle_transfer_busy_limit);
}

void SignalHandler(int signo) {
  quit_program = true;
}

}  // anonymous namespace
