/*
 * System V shared memory for the desktop (preloaded into the X server and every app).
 * Android's kernel has none, so X programs fall back to pushing every picture down the X socket
 * in strips — Firefox repaints visibly sweep down the screen. This gives them shmget / shmat /
 * shmdt / shmctl backed by files in /dev/shm (shared by everything in the session), so MIT-SHM
 * works: the app draws straight into memory the X server reads.
 *
 * A segment is /dev/shm/.sysv-<id>. Removing one (IPC_RMID) deletes its file once this process
 * has let go of it, so the X server can still attach a segment its app already removed (as
 * Linux allows). Keyed segments (rare in X programs) are not supported.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/*
 * And one fix for Windows programs: Wine loads a .dll by mapping its file, patching it, then
 * marking its code executable — which Android's app policy forbids for a patched file mapping
 * (SELinux execmod), so every program died with "noexec filesystem?". Under Box64 the x86 code
 * is never run natively (Box64 translates it into its own memory), so when that call is refused
 * it's made again without the executable bit; Box64 still treats the pages as code.
 */
#include <dlfcn.h>
int mprotect(void *addr, size_t len, int prot) {
    static int (*real)(void *, size_t, int);
    if (!real) real = (int (*)(void *, size_t, int))dlsym(RTLD_NEXT, "mprotect");
    int r = real(addr, len, prot);
    if (r < 0 && errno == EACCES && (prot & PROT_EXEC)) r = real(addr, len, prot & ~PROT_EXEC);
    return r;
}

#define MAXMAP 256

typedef struct { void *addr; size_t size; int id; } Map;
static Map maps[MAXMAP];
static int removed[MAXMAP], nremoved;   /* removed, waiting for this process to let go */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void segPath(int id, char *p, size_t n) { snprintf(p, n, "/dev/shm/.sysv-%d", id); }

static int attachedHere(int id) {
    for (int i = 0; i < MAXMAP; i++) if (maps[i].addr && maps[i].id == id) return 1;
    return 0;
}

int shmget(key_t key, size_t size, int flags) {
    if (key != IPC_PRIVATE) { errno = ENOSYS; return -1; }
    if (!size) { errno = EINVAL; return -1; }
    char p[64];
    unsigned seed = (unsigned)getpid() * 2654435761u ^ (unsigned)time(NULL) ^ (unsigned)(size_t)&p;
    for (int tries = 0; tries < 64; tries++) {
        seed = seed * 1103515245u + 12345u;
        int id = (int)((seed >> 1) & 0x3fffffff) | 1;
        segPath(id, p, sizeof p);
        int fd = open(p, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
        if (fd < 0) { if (errno == EEXIST) continue; return -1; }
        if (ftruncate(fd, (off_t)size) < 0) { int e = errno; close(fd); unlink(p); errno = e; return -1; }
        close(fd);
        return id;
    }
    errno = ENOSPC;
    return -1;
}

void *shmat(int id, const void *where, int flags) {
    char p[64];
    segPath(id, p, sizeof p);
    int ro = flags & SHM_RDONLY;
    int fd = open(p, (ro ? O_RDONLY : O_RDWR) | O_CLOEXEC);
    if (fd < 0) { errno = EINVAL; return (void *)-1; }
    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); errno = EINVAL; return (void *)-1; }
    void *a = mmap((void *)where, (size_t)st.st_size, ro ? PROT_READ : PROT_READ | PROT_WRITE,
                   MAP_SHARED | (where ? MAP_FIXED : 0), fd, 0);
    close(fd);
    if (a == MAP_FAILED) { errno = ENOMEM; return (void *)-1; }
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAXMAP; i++) if (!maps[i].addr) { maps[i] = (Map){ a, (size_t)st.st_size, id }; break; }
    pthread_mutex_unlock(&lock);
    return a;
}

int shmdt(const void *addr) {
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAXMAP; i++) {
        if (maps[i].addr != addr) continue;
        munmap(maps[i].addr, maps[i].size);
        int id = maps[i].id;
        maps[i].addr = NULL;
        /* removed while attached: now it can go */
        for (int r = 0; r < nremoved; r++) if (removed[r] == id && !attachedHere(id)) {
            char p[64]; segPath(id, p, sizeof p); unlink(p);
            removed[r] = removed[--nremoved];
            break;
        }
        pthread_mutex_unlock(&lock);
        return 0;
    }
    pthread_mutex_unlock(&lock);
    errno = EINVAL;
    return -1;
}

/* Exiting: segments this process removed go now (anyone still attached keeps their mapping). */
__attribute__((destructor)) static void cleanup(void) {
    for (int r = 0; r < nremoved; r++) { char p[64]; segPath(removed[r], p, sizeof p); unlink(p); }
    nremoved = 0;
}

int shmctl(int id, int cmd, struct shmid_ds *buf) {
    char p[64];
    segPath(id, p, sizeof p);
    struct stat st;
    if (id < 0 || stat(p, &st) < 0) { errno = EINVAL; return -1; }
    switch (cmd) {
    case IPC_STAT:
        if (!buf) { errno = EFAULT; return -1; }
        memset(buf, 0, sizeof *buf);
        buf->shm_segsz = (size_t)st.st_size;
        buf->shm_perm.uid = buf->shm_perm.cuid = getuid();
        buf->shm_perm.gid = buf->shm_perm.cgid = getgid();
        buf->shm_perm.mode = 0666;
        buf->shm_nattch = 1;
        buf->shm_cpid = buf->shm_lpid = getpid();
        return 0;
    case IPC_RMID:
        pthread_mutex_lock(&lock);
        if (attachedHere(id)) { if (nremoved < MAXMAP) removed[nremoved++] = id; }
        else unlink(p);
        pthread_mutex_unlock(&lock);
        return 0;
    case IPC_SET:
        return 0;
    default:
        errno = EINVAL;
        return -1;
    }
}
