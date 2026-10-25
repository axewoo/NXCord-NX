// POSIX bits that OpenSSL needs but newlib/libnx don't provide.
#include <switch.h>
#include <errno.h>
#include <stddef.h>
#include <sys/types.h>

// OpenSSL looks up getentropy() as a weak symbol, so define it here to make
// sure it's linked. Backed by libnx's kernel-seeded CSPRNG.
int getentropy(void *buf, size_t len) {
    randomGet(buf, len);
    return 0;
}

uid_t getuid(void) { return 0; }
uid_t geteuid(void) { return 0; }
gid_t getgid(void) { return 0; }
gid_t getegid(void) { return 0; }
