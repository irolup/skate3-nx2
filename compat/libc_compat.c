// libc functions the SDK uses that newlib/libounce does not provide.
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stddef.h>
#include <sys/types.h>
#include <unistd.h>
#include <switch.h>

long sysconf(int name) {
  switch (name) {
    case _SC_PAGESIZE: return 4096;
    case _SC_NPROCESSORS_ONLN:
    case _SC_NPROCESSORS_CONF: return 6;
    default: errno = EINVAL; return -1;
  }
}

static pthread_mutex_t g_pio = PTHREAD_MUTEX_INITIALIZER;

ssize_t pread(int fd, void* buf, size_t n, off_t off) {
  pthread_mutex_lock(&g_pio);
  off_t cur = lseek(fd, 0, SEEK_CUR);
  ssize_t r = -1;
  if (lseek(fd, off, SEEK_SET) >= 0) r = read(fd, buf, n);
  lseek(fd, cur, SEEK_SET);
  pthread_mutex_unlock(&g_pio);
  return r;
}

ssize_t pwrite(int fd, const void* buf, size_t n, off_t off) {
  pthread_mutex_lock(&g_pio);
  off_t cur = lseek(fd, 0, SEEK_CUR);
  ssize_t r = -1;
  if (lseek(fd, off, SEEK_SET) >= 0) r = write(fd, buf, n);
  lseek(fd, cur, SEEK_SET);
  pthread_mutex_unlock(&g_pio);
  return r;
}

int creat(const char* path, mode_t mode) { return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode); }

struct _reent;
int _getentropy_r(struct _reent* r, void* buf, size_t len) {
  (void)r;
  randomGet(buf, len);
  return 0;
}

#include <pwd.h>
// No user database on Horizon.
int getpwuid_r(uid_t uid, struct passwd* pwd, char* buf, size_t buflen, struct passwd** result) {
  (void)uid; (void)pwd; (void)buf; (void)buflen;
  *result = 0;
  return ENOENT;
}
