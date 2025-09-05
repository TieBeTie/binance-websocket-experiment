#pragma once

#include <errno.h>
#include <sys/uio.h>
#include <thread>
#include <unistd.h>

namespace io {

inline void WriteAll(int fd, const char *data, std::size_t len) {
  const char *p = data;
  std::size_t remaining = len;
  while (remaining > 0) {
    ssize_t n = ::write(fd, p, remaining);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        std::this_thread::yield();
        continue;
      }
      break;
    }
    p += static_cast<std::size_t>(n);
    remaining -= static_cast<std::size_t>(n);
  }
}

inline void WritevAll(int fd, struct iovec *iov, int cnt) {
  while (cnt > 0) {
    ssize_t n = ::writev(fd, iov, cnt);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        std::this_thread::yield();
        continue;
      }
      break;
    }
    ssize_t consumed = n;
    while (consumed > 0 && cnt > 0) {
      if (consumed >= static_cast<ssize_t>(iov[0].iov_len)) {
        consumed -= static_cast<ssize_t>(iov[0].iov_len);
        ++iov;
        --cnt;
      } else {
        iov[0].iov_base = static_cast<char *>(iov[0].iov_base) + consumed;
        iov[0].iov_len -= static_cast<size_t>(consumed);
        consumed = 0;
      }
    }
  }
}

} // namespace io
