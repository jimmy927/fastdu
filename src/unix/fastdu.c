/*
 * fastdu for Linux and macOS: GNU du, on every core.
 *
 * The walk is the same on both; only listing a folder differs (`list`):
 *   - a Linux listing (getdents64) has names, inode numbers and types but no
 *     sizes, so files need a stat, each with fstatat relative to its folder's
 *     descriptor so the kernel resolves one name, not a whole path;
 *   - a macOS listing (getattrlistbulk) can carry each file's size, link count
 *     and times with its name, as Windows' does, so nothing is stat-ed.
 * Around that:
 *   - a thread walks its own subtree depth-first and hands folders to a shared
 *     stack only while another thread is idle;
 *   - nothing is allocated per file (but with -a); folder names go into
 *     per-thread arenas.
 * Counted as du counts: allocated blocks (or apparent sizes), a file with
 * several hard links once, other file systems too unless -x, symbolic links
 * followed only as -D and -L say. The command line is in `options.h`.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/syscall.h>
#elif defined(__APPLE__)
#include <sys/attr.h>
#include <sys/resource.h>
#include <sys/vnode.h>
#else
#error "fastdu is for Linux, macOS and Windows"
#endif

#include "../options.h"
#include "../output.h"

#if defined(__linux__)
struct linux_dirent64 {
    uint64_t d_ino;
    int64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
};
#endif

static Options opt;
static int pathsForExcludes; /* a pattern with a slash: match whole paths */
static atomic_int failed;

/* ---- per-thread memory ---- */

typedef struct {
    char *at;
    size_t left;
} Arena;

static void *take(Arena *arena, size_t size) {
    size = (size + 15) & ~(size_t)15;
    if (arena->left < size) {
        size_t chunk = size > (4u << 20) ? size : (4u << 20);
        arena->at = malloc(chunk);
        if (arena->at == NULL) {
            fputs("fastdu: memory exhausted\n", stderr);
            exit(1);
        }
        arena->left = chunk;
    }
    void *out = arena->at;
    arena->at += size;
    arena->left -= size;
    return out;
}

/* ---- the tree ---- */

typedef struct Node {
    struct Node *parent;
    struct Node *child, *sibling; /* the printed tree, linked up at the end */
    char *name; /* its own name; a root's is its path as given */
    int depth;
    unsigned char isDir;
    unsigned char follow; /* opened following a symbolic link (-D, -L) */
    unsigned char printed; /* it has a line */
    dev_t device; /* its file system */
    long long ownBytes, ownFiles, ownTime; /* itself and its files, for -S */
    atomic_llong bytes, files, dirs, time; /* its subtree, once complete */
    atomic_int pending; /* subfolders not yet complete, plus its own listing */
} Node;

static pthread_mutex_t shownLock = PTHREAD_MUTEX_INITIALIZER;
static Node **shown;
static size_t shownCount, shownCap;

static int shownAt(int depth) { return opt.depth < 0 || depth <= opt.depth; }

static void show(Node *node) {
    node->printed = 1;
    pthread_mutex_lock(&shownLock);
    if (shownCount == shownCap) {
        shownCap = shownCap ? shownCap * 2 : 4096;
        shown = realloc(shown, shownCap * sizeof *shown);
    }
    shown[shownCount++] = node;
    pthread_mutex_unlock(&shownLock);
}

static void timeMax(atomic_llong *into, long long value) {
    long long seen = atomic_load(into);
    while (value > seen && !atomic_compare_exchange_weak(into, &seen, value)) {
    }
}

/* A folder whose listing and every subfolder are done adds itself to its parent. */
static void complete(Node *node) {
    while (node != NULL) {
        if (atomic_fetch_sub(&node->pending, 1) != 1) return;
        Node *parent = node->parent;
        if (parent == NULL) return;
        atomic_fetch_add(&parent->bytes, atomic_load(&node->bytes));
        atomic_fetch_add(&parent->files, atomic_load(&node->files));
        atomic_fetch_add(&parent->dirs, atomic_load(&node->dirs));
        timeMax(&parent->time, atomic_load(&node->time));
        node = parent;
    }
}

/*
 * ---- every file seen, by file system and inode: each counted once ----
 * A file with several names (hard links) is counted at the first name met,
 * as du does (not with -l). Every file goes in, not only those known to have
 * several names: on Linux the check comes before the stat, which it saves.
 * With -L, folders go in too, so a loop of links is walked once.
 */

#define SHARDS 256
typedef struct {
    uint64_t device;
    uint64_t ino; /* plus one: 0 marks an empty slot */
} Seen;
typedef struct {
    pthread_mutex_t lock;
    Seen *slots;
    size_t cap, used;
} Shard;
static Shard seen[SHARDS];

static uint64_t hashOf(uint64_t device, uint64_t ino) {
    return (ino * 0x9E3779B97F4A7C15ull) ^ (device * 0xC2B2AE3D27D4EB4Full);
}

/* True the first time this file is seen. */
static int firstSight(uint64_t device, uint64_t ino) {
    Seen key = {device, ino + 1};
    uint64_t hash = hashOf(device, key.ino);
    Shard *shard = &seen[hash >> 56];
    pthread_mutex_lock(&shard->lock);
    if (shard->used * 2 >= shard->cap) {
        size_t cap = shard->cap ? shard->cap * 2 : 1024;
        Seen *slots = calloc(cap, sizeof *slots);
        for (size_t i = 0; i < shard->cap; i++) {
            Seen old = shard->slots[i];
            if (old.ino == 0) continue;
            size_t at = hashOf(old.device, old.ino) & (cap - 1);
            while (slots[at].ino != 0) at = (at + 1) & (cap - 1);
            slots[at] = old;
        }
        free(shard->slots);
        shard->slots = slots;
        shard->cap = cap;
    }
    size_t at = hash & (shard->cap - 1);
    while (shard->slots[at].ino != 0) {
        if (shard->slots[at].ino == key.ino && shard->slots[at].device == key.device) {
            pthread_mutex_unlock(&shard->lock);
            return 0;
        }
        at = (at + 1) & (shard->cap - 1);
    }
    shard->slots[at] = key;
    shard->used++;
    pthread_mutex_unlock(&shard->lock);
    return 1;
}

/* ---- shared work ---- */

static pthread_mutex_t workLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t workReady = PTHREAD_COND_INITIALIZER;
static Node **work;
static size_t workCount, workCap;
static atomic_int idle;
static int threads;
static int finished;

static atomic_llong totalDirs, totalFiles, totalBytes;

static void give(Node *node) {
    pthread_mutex_lock(&workLock);
    if (workCount == workCap) {
        workCap = workCap ? workCap * 2 : 4096;
        work = realloc(work, workCap * sizeof *work);
    }
    work[workCount++] = node;
    pthread_mutex_unlock(&workLock);
    pthread_cond_signal(&workReady);
}

#define BUFFER (1 << 20)

typedef struct {
    Arena arena;
    char *buffer;
} Worker;

static Node *newNode(Arena *arena, Node *parent, const char *name, size_t length, int isDir) {
    Node *node = take(arena, sizeof(Node));
    memset(node, 0, sizeof *node);
    node->parent = parent;
    node->name = take(arena, length + 1);
    memcpy(node->name, name, length);
    node->name[length] = 0;
    node->depth = parent == NULL ? 0 : parent->depth + 1;
    node->device = parent == NULL ? 0 : parent->device;
    node->isDir = (unsigned char)isDir;
    atomic_init(&node->bytes, 0);
    atomic_init(&node->files, 0);
    atomic_init(&node->dirs, 0);
    atomic_init(&node->time, 0);
    atomic_init(&node->pending, 1);
    return node;
}

/* A node's path as printed: its root as given, then each name. */
static char *fullPath(Node *node, Arena *arena) {
    Node *chain[4096];
    int depth = 0;
    size_t length = 0;
    for (Node *at = node; at != NULL && depth < 4096; at = at->parent) {
        chain[depth++] = at;
        length += strlen(at->name) + 1;
    }
    char *out = take(arena, length + 1);
    size_t used = 0;
    for (int i = depth - 1; i >= 0; i--) {
        size_t n = strlen(chain[i]->name);
        if (i != depth - 1 && used > 0 && out[used - 1] != '/') out[used++] = '/';
        memcpy(out + used, chain[i]->name, n);
        used += n;
    }
    out[used] = 0;
    return out;
}

/* A child's path, for a pattern with a slash in it; only then is it built. */
static int excludedChild(Worker *self, Node *parent, const char *name) {
    if (opt.excludeCount == 0) return 0;
    if (!pathsForExcludes) return excluded(&opt, name);
    char *above = fullPath(parent, &self->arena);
    size_t a = strlen(above), n = strlen(name);
    char *path = take(&self->arena, a + n + 2);
    memcpy(path, above, a);
    size_t used = a;
    if (used > 0 && path[used - 1] != '/') path[used++] = '/';
    memcpy(path + used, name, n + 1);
    return excluded(&opt, path);
}

static void complain(const char *what, const char *path, int error) {
    fprintf(stderr, "fastdu: %s '%s': %s\n", what, path, strerror(error));
    atomic_store(&failed, 1);
}

/* ---- what a stat says, as du counts it ---- */

static long long nanosOf(struct timespec t) { return (long long)t.tv_sec * 1000000000LL + t.tv_nsec; }

static long long sizeOf(const struct stat *info) {
    return opt.apparent ? (long long)info->st_size : (long long)info->st_blocks * 512;
}

static long long timeOf(const struct stat *info) {
#if defined(__APPLE__)
    struct timespec m = info->st_mtimespec, a = info->st_atimespec, c = info->st_ctimespec;
#else
    struct timespec m = info->st_mtim, a = info->st_atim, c = info->st_ctim;
#endif
    if (opt.time == TIME_ACCESSED) return nanosOf(a);
    if (opt.time == TIME_CHANGED) return nanosOf(c);
    return nanosOf(m);
}

/* What one folder's listing adds up to: its files, and its subfolders to walk. */
typedef struct {
    Node **items;
    size_t count, cap;
    long long bytes, files, time;
} Listing;

static Node *addChild(Worker *self, Node *node, const char *name, Listing *listing, int follow) {
    if (listing->count == listing->cap) {
        size_t grown = listing->cap ? listing->cap * 2 : 16;
        Node **more = take(&self->arena, grown * sizeof *more);
        if (listing->count) memcpy(more, listing->items, listing->count * sizeof *more);
        listing->items = more;
        listing->cap = grown;
    }
    Node *child = newNode(&self->arena, node, name, strlen(name), 1);
    child->follow = (unsigned char)follow;
    listing->items[listing->count++] = child;
    return child;
}

/* A file counted in this folder: its size, and with -a, a line of its own. */
static void addFile(Worker *self, Node *node, const char *name, long long bytes, long long time,
                    Listing *listing) {
    listing->bytes += bytes;
    listing->files += 1;
    if (time > listing->time) listing->time = time;
    if (!opt.all || !shownAt(node->depth + 1)) return;
    Node *file = newNode(&self->arena, node, name, strlen(name), 0);
    file->ownBytes = bytes;
    file->ownFiles = 1;
    file->ownTime = time;
    atomic_init(&file->bytes, bytes);
    atomic_init(&file->files, 1);
    atomic_init(&file->time, time);
    atomic_init(&file->pending, 0);
    show(file);
}

/*
 * An entry that is a symbolic link, with -L: what it points to. A folder is
 * walked as a child; a file counts once by its own inode; a dangling link is
 * reported and not counted, as du does ("cannot access", with no reason).
 */
static void followLink(Worker *self, Node *node, int fd, const char *name, Listing *listing) {
    struct stat target;
    if (fstatat(fd, name, &target, 0) != 0) {
        char *above = fullPath(node, &self->arena);
        size_t length = strlen(above);
        const char *slash = length > 0 && above[length - 1] == '/' ? "" : "/";
        fprintf(stderr, "fastdu: cannot access '%s%s%s'\n", above, slash, name);
        atomic_store(&failed, 1);
        return;
    }
    if (S_ISDIR(target.st_mode)) {
        addChild(self, node, name, listing, 1);
        return;
    }
    if (!opt.countLinks && !firstSight(target.st_dev, target.st_ino)) return;
    addFile(self, node, name, sizeOf(&target), timeOf(&target), listing);
}

#if defined(__linux__)
/*
 * List one folder: its subfolders, and its files' sizes and count.
 *
 * The stats are what cost: 4.3 µs each on ~/src, 70 % of a one-thread walk,
 * where listing alone took 2.4 s of 8.2 (2026-09-27). So as few as possible:
 *   - a folder is not stat-ed from its parent. Its own blocks and file system
 *     come from fstat on the descriptor it is listed through (`walk`);
 *   - a file whose inode number (which the listing gives for free) was already
 *     seen is another name for a file already counted, and is skipped without
 *     a stat. ~/src had 2.49 M file names for 1.15 M files: node_modules is
 *     hard-linked between checkouts.
 */
static int list(Worker *self, Node *node, int fd, Listing *listing) {
    for (;;) {
        long got = syscall(SYS_getdents64, fd, self->buffer, BUFFER);
        if (got < 0) return errno;
        if (got == 0) return 0;
        /*
         * A read that left a page of the buffer unused was the whole folder,
         * so the call that would only answer "end" is skipped: one system call
         * fewer per folder. Checked on ~/src, ~ and /tmp (6.6 M files): the
         * same counts as reading to the end, but for files written between
         * the two runs. Unlike NTFS (see the Windows fastdu), ext4 fills the
         * buffer as far as the folder goes.
         */
        int whole = got < BUFFER - 4096;
        for (long at = 0; at < got;) {
            struct linux_dirent64 *entry = (struct linux_dirent64 *)(self->buffer + at);
            at += entry->d_reclen;
            const char *name = entry->d_name;
            if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) continue;
            if (excludedChild(self, node, name)) continue;
            unsigned char type = entry->d_type;
            struct stat info;
            int statted = 0;
            /* The file system does not say what it is: ask. */
            if (type == DT_UNKNOWN) {
                if (fstatat(fd, name, &info, AT_SYMLINK_NOFOLLOW) != 0) continue;
                statted = 1;
                type = S_ISDIR(info.st_mode) ? DT_DIR : S_ISLNK(info.st_mode) ? DT_LNK : DT_REG;
            }
            if (type == DT_DIR) {
                addChild(self, node, name, listing, 0);
                continue;
            }
            if (type == DT_LNK && opt.deref) {
                followLink(self, node, fd, name, listing);
                continue;
            }
            if (!opt.countLinks && !firstSight(node->device, entry->d_ino)) continue;
            if (!statted && fstatat(fd, name, &info, AT_SYMLINK_NOFOLLOW) != 0) continue;
            addFile(self, node, name, sizeOf(&info), timeOf(&info), listing);
        }
        if (whole) return 0;
    }
}
#elif defined(__APPLE__)
/*
 * List one folder: its subfolders, and its files' sizes and count.
 *
 * getattrlistbulk returns, with each name, what is asked of it: here its
 * kind, file ID, link count, allocated or apparent size and the time --time
 * wants. So no file is stat-ed, and only a file with several names is looked
 * up in the set of files seen. Each entry holds only the attributes it
 * returned, in the order of their bits, 4-byte aligned, so they are read one
 * after another as `returned` says.
 */
static int list(Worker *self, Node *node, int fd, Listing *listing) {
    struct attrlist request;
    memset(&request, 0, sizeof request);
    request.bitmapcount = ATTR_BIT_MAP_COUNT;
    attrgroup_t timeAttr = opt.time == TIME_ACCESSED  ? ATTR_CMN_ACCTIME
                           : opt.time == TIME_CHANGED ? ATTR_CMN_CHGTIME
                                                      : ATTR_CMN_MODTIME;
    request.commonattr = ATTR_CMN_RETURNED_ATTRS | ATTR_CMN_ERROR | ATTR_CMN_NAME |
                         ATTR_CMN_OBJTYPE | ATTR_CMN_FILEID;
    if (opt.time != TIME_NONE) request.commonattr |= timeAttr;
    request.fileattr = ATTR_FILE_LINKCOUNT | ATTR_FILE_ALLOCSIZE;
    if (opt.apparent) request.fileattr |= ATTR_FILE_DATALENGTH;
    for (;;) {
        int got = getattrlistbulk(fd, &request, self->buffer, BUFFER, 0);
        if (got < 0) return errno;
        if (got == 0) return 0;
        char *entry = self->buffer;
        for (int i = 0; i < got; i++) {
            uint32_t length;
            memcpy(&length, entry, sizeof length);
            char *field = entry + sizeof length;
            entry += length;
            attribute_set_t returned;
            memcpy(&returned, field, sizeof returned);
            field += sizeof returned;
            uint32_t error = 0;
            if (returned.commonattr & ATTR_CMN_ERROR) {
                memcpy(&error, field, sizeof error);
                field += sizeof error;
            }
            const char *name = NULL;
            if (returned.commonattr & ATTR_CMN_NAME) {
                attrreference_t reference;
                memcpy(&reference, field, sizeof reference);
                name = field + reference.attr_dataoffset;
                field += sizeof reference;
            }
            fsobj_type_t type = VNON;
            if (returned.commonattr & ATTR_CMN_OBJTYPE) {
                memcpy(&type, field, sizeof type);
                field += sizeof type;
            }
            /* The three times' bits come in this order; only the asked one is there. */
            struct timespec when = {0, 0};
            attrgroup_t times[] = {ATTR_CMN_MODTIME, ATTR_CMN_CHGTIME, ATTR_CMN_ACCTIME};
            for (int t = 0; t < 3; t++) {
                if (!(returned.commonattr & times[t])) continue;
                memcpy(&when, field, sizeof when);
                field += sizeof when;
            }
            uint64_t id = 0;
            if (returned.commonattr & ATTR_CMN_FILEID) {
                memcpy(&id, field, sizeof id);
                field += sizeof id;
            }
            uint32_t links = 1;
            if (returned.fileattr & ATTR_FILE_LINKCOUNT) {
                memcpy(&links, field, sizeof links);
                field += sizeof links;
            }
            off_t allocated = 0, data = 0;
            if (returned.fileattr & ATTR_FILE_ALLOCSIZE) {
                memcpy(&allocated, field, sizeof allocated);
                field += sizeof allocated;
            }
            if (returned.fileattr & ATTR_FILE_DATALENGTH) memcpy(&data, field, sizeof data);
            if (error != 0 || name == NULL) continue;
            if (excludedChild(self, node, name)) continue;
            if (type == VDIR) {
                addChild(self, node, name, listing, 0);
                continue;
            }
            if (type == VLNK && opt.deref) {
                followLink(self, node, fd, name, listing);
                continue;
            }
            if (links > 1 && !opt.countLinks && !firstSight(node->device, id)) continue;
            long long size = opt.apparent ? (long long)data : (long long)allocated;
            addFile(self, node, name, size, nanosOf(when), listing);
        }
    }
}
#endif

/* A folder counted as itself only: it could not be opened, or read. */
static void folderAlone(Node *node, const struct stat *info) {
    if (info != NULL) {
        node->ownBytes = sizeOf(info);
        node->ownTime = timeOf(info);
    }
    atomic_store(&node->bytes, node->ownBytes);
    atomic_store(&node->dirs, 1);
    atomic_store(&node->time, node->ownTime);
    if (shownAt(node->depth)) show(node);
}

/*
 * openat, again when interrupted. On macOS 14 and later, opening another app's
 * container asks the privacy service for approval; with several asks in flight
 * one sometimes went unanswered until the kernel's 5 s watchdog ended it with
 * EINTR ("watchdog expired for approval entry … kTCCServiceSystemPolicyAppData",
 * 2026-09-27). Asked again, the answer is there: "Operation not permitted",
 * what du reports for the same folder.
 */
static int openFolder(int at, const char *name, int flags) {
    int fd;
    int tries = 0;
    do fd = openat(at, name, flags);
    while (fd < 0 && errno == EINTR && ++tries < 3);
    return fd;
}

static void walk(Worker *self, Node *node, int fd) {
    struct stat own;
    if (fstat(fd, &own) != 0) {
        close(fd);
        complete(node);
        return;
    }
    /* Another file system mounted here, with -x: left out, as du leaves it. */
    if (opt.oneFileSystem && node->parent != NULL && own.st_dev != node->device) {
        close(fd);
        complete(node);
        return;
    }
    /* With -L a folder can be reached twice: walked the first time only. */
    if (opt.deref && !firstSight(own.st_dev, own.st_ino)) {
        close(fd);
        complete(node);
        return;
    }
    node->device = own.st_dev;
    Listing listing;
    memset(&listing, 0, sizeof listing);
    int error = list(self, node, fd, &listing);
    if (error != 0) complain("cannot read directory", fullPath(node, &self->arena), error);
    long long ownTime = timeOf(&own);
    node->ownBytes = sizeOf(&own) + listing.bytes;
    node->ownFiles = listing.files;
    node->ownTime = listing.time > ownTime ? listing.time : ownTime;
    atomic_fetch_add(&node->bytes, node->ownBytes);
    atomic_fetch_add(&node->files, node->ownFiles);
    atomic_fetch_add(&node->dirs, 1);
    timeMax(&node->time, node->ownTime);
    atomic_fetch_add(&totalDirs, 1);
    atomic_fetch_add(&totalFiles, listing.files);
    atomic_fetch_add(&totalBytes, node->ownBytes);
    if (shownAt(node->depth)) show(node);
    atomic_fetch_add(&node->pending, (int)listing.count);
    for (size_t i = 0; i < listing.count; i++) {
        Node *child = listing.items[i];
        if (atomic_load(&idle) > 0 && i + 1 < listing.count) {
            give(child);
            continue;
        }
        int flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | (child->follow ? 0 : O_NOFOLLOW);
        int sub = openFolder(fd, child->name, flags);
        if (sub < 0) {
            int why = errno;
            complain("cannot read directory", fullPath(child, &self->arena), why);
            struct stat info;
            int follow = child->follow ? 0 : AT_SYMLINK_NOFOLLOW;
            folderAlone(child, fstatat(fd, child->name, &info, follow) == 0 ? &info : NULL);
            complete(child);
            continue;
        }
        walk(self, child, sub);
    }
    close(fd);
    complete(node);
}

static void *worker(void *unused) {
    (void)unused;
    Worker self = {{NULL, 0}, malloc(BUFFER)};
    for (;;) {
        pthread_mutex_lock(&workLock);
        atomic_fetch_add(&idle, 1);
        while (workCount == 0) {
            if (finished || atomic_load(&idle) == threads) {
                finished = 1;
                pthread_mutex_unlock(&workLock);
                pthread_cond_broadcast(&workReady);
                return NULL;
            }
            pthread_cond_wait(&workReady, &workLock);
        }
        Node *node = work[--workCount];
        atomic_fetch_sub(&idle, 1);
        pthread_mutex_unlock(&workLock);
        char *path = fullPath(node, &self.arena);
        int flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | (node->follow ? 0 : O_NOFOLLOW);
        int fd = openFolder(AT_FDCWD, path, flags);
        if (fd < 0) {
            complain("cannot read directory", path, errno);
            struct stat info;
            int got = node->follow ? stat(path, &info) : lstat(path, &info);
            folderAlone(node, got == 0 ? &info : NULL);
            complete(node);
            continue;
        }
        walk(&self, node, fd);
    }
}

#if defined(__APPLE__)
/*
 * More threads while the ones there are mostly wait. One thread per processor
 * suits a walk whose file system answers from memory, but where it waits on
 * the disk more calls in flight pay: on a 3-processor Apple Silicon runner, all
 * of / (3.24 M files) took 72.6 s at 3 threads and 33.1 s at 16, while on an
 * 8-thread Intel Mac with everything cached, 16 threads were only ~10 % faster
 * (5.5-6.0 s against 6.5-7.1 s; 2026-09-27). So every 100 ms: if every thread
 * is walking and together they used under half the processor time they had —
 * waiting, not working — another processor's worth of threads starts, up to 8
 * per processor.
 */
static double seconds(struct timeval t) { return (double)t.tv_sec + t.tv_usec / 1e6; }

static double cpuSeconds(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return seconds(usage.ru_utime) + seconds(usage.ru_stime);
}

static double wallSeconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + now.tv_nsec / 1e9;
}

static void *grower(void *processors) {
    int step = *(int *)processors;
    int cap = 8 * step;
    struct timespec pause = {0, 100 * 1000 * 1000};
    double cpu = cpuSeconds(), wall = wallSeconds();
    for (;;) {
        nanosleep(&pause, NULL);
        double nowCpu = cpuSeconds(), nowWall = wallSeconds();
        double used = (nowCpu - cpu) / (nowWall - wall);
        cpu = nowCpu;
        wall = nowWall;
        pthread_mutex_lock(&workLock);
        if (finished) {
            pthread_mutex_unlock(&workLock);
            return NULL;
        }
        int all = threads, walking = threads - atomic_load(&idle);
        int grow = walking == all && all < cap && used < 0.5 * all;
        if (grow) threads += step;
        pthread_mutex_unlock(&workLock);
        for (int i = 0; grow && i < step; i++) {
            pthread_t extra;
            pthread_create(&extra, NULL, worker, NULL);
            pthread_detach(extra);
        }
    }
}
#endif

static void *reporter(void *unused) {
    (void)unused;
    for (;;) {
        sleep(1);
        fprintf(stderr, "progress\t%lld\t%lld\t%lld\n", atomic_load(&totalDirs),
                atomic_load(&totalFiles), atomic_load(&totalBytes));
        fflush(stderr);
    }
    return NULL;
}

/* ---- printing, in du's order: a folder after everything in it ---- */

static void printPath(FILE *out, Node *node) {
    if (node->parent != NULL) {
        printPath(out, node->parent);
        const char *above = node->parent->name;
        if (above[strlen(above) - 1] != '/') fputc('/', out);
    }
    fputs(node->name, out);
}

/* What a line counts: bytes or inodes, the subtree's or with -S its own. */
static long long amountOf(Node *node) {
    int own = opt.separateDirs && node->isDir;
    if (opt.inodes) {
        return own ? node->ownFiles + 1
                   : atomic_load(&node->files) + atomic_load(&node->dirs);
    }
    return own ? node->ownBytes : atomic_load(&node->bytes);
}

static long long filesOf(Node *node) {
    return opt.separateDirs && node->isDir ? node->ownFiles : atomic_load(&node->files);
}

static long long latestOf(Node *node) {
    return opt.separateDirs && node->isDir ? node->ownTime : atomic_load(&node->time);
}

static void printNode(FILE *out, Node *node) {
    long long amount = amountOf(node);
    if (!shownBy(&opt, amount)) return;
    printLead(out, &opt, amount, filesOf(node), latestOf(node));
    printPath(out, node);
    endLine(out, &opt);
}

static void printTree(FILE *out, Node *root) {
    /* Iterative post-order: a node is printed once its children are. */
    size_t cap = 1024, depth = 0;
    Node **stack = malloc(cap * sizeof *stack);
    char *entered = malloc(cap);
    stack[0] = root;
    entered[0] = 0;
    depth = 1;
    while (depth > 0) {
        Node *node = stack[depth - 1];
        if (entered[depth - 1]) {
            printNode(out, node);
            depth--;
            continue;
        }
        entered[depth - 1] = 1;
        for (Node *child = node->child; child != NULL; child = child->sibling) {
            if (depth == cap) {
                cap *= 2;
                stack = realloc(stack, cap * sizeof *stack);
                entered = realloc(entered, cap);
            }
            stack[depth] = child;
            entered[depth] = 0;
            depth++;
        }
    }
    free(stack);
    free(entered);
}

int main(int argc, char **argv) {
    parseOptions(argc, argv, &opt);
    pathsForExcludes = excludesNeedPaths(&opt);
    threads = opt.threads > 0 ? opt.threads : (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (threads < 1) threads = 1;
    for (int i = 0; i < SHARDS; i++) pthread_mutex_init(&seen[i].lock, NULL);
    Arena main = {NULL, 0};
    Node **roots = malloc((size_t)opt.rootCount * sizeof *roots);
    int rootCount = 0;
    for (int i = 0; i < opt.rootCount; i++) {
        char *root = opt.roots[i];
        int follow = opt.derefArgs || opt.deref;
        struct stat info;
        if ((follow ? stat(root, &info) : lstat(root, &info)) != 0) {
            complain("cannot access", root, errno);
            continue;
        }
        Node *node = newNode(&main, NULL, root, strlen(root), S_ISDIR(info.st_mode));
        node->device = info.st_dev;
        node->follow = (unsigned char)follow;
        roots[rootCount++] = node;
        /* A file given is printed as itself, as du does, and counted once. */
        if (!S_ISDIR(info.st_mode)) {
            atomic_init(&node->pending, 0);
            if (!opt.countLinks && !firstSight(info.st_dev, info.st_ino)) continue;
            node->ownBytes = sizeOf(&info);
            node->ownFiles = 1;
            node->ownTime = timeOf(&info);
            atomic_init(&node->bytes, node->ownBytes);
            atomic_init(&node->files, 1);
            atomic_init(&node->time, node->ownTime);
            show(node);
            continue;
        }
        give(node);
    }
    if (opt.progress) {
        pthread_t tick;
        pthread_create(&tick, NULL, reporter, NULL);
    }
    int started = threads;
    pthread_t *pool = malloc((size_t)started * sizeof *pool);
    for (int i = 0; i < started; i++) pthread_create(&pool[i], NULL, worker, NULL);
#if defined(__APPLE__)
    /* Threads it adds are detached: the walk is over only once all are idle. */
    if (opt.threads == 0) {
        pthread_t growing;
        pthread_create(&growing, NULL, grower, &started);
        pthread_detach(growing);
    }
#endif
    for (int i = 0; i < started; i++) pthread_join(pool[i], NULL);

    /* Link up the printed tree: each shown node under its parent. */
    for (size_t i = shownCount; i-- > 0;) {
        Node *node = shown[i];
        if (node->parent == NULL) continue;
        node->sibling = node->parent->child;
        node->parent->child = node;
    }
    static char out[1 << 22];
    setvbuf(stdout, out, _IOFBF, sizeof out);
    long long total = 0, totalFilesCount = 0, latest = 0;
    /* The total is every root's whole subtree, with -S too, as du's is. */
    for (int i = 0; i < rootCount; i++) {
        Node *root = roots[i];
        if (!root->printed) continue;
        printTree(stdout, root);
        total += opt.inodes ? atomic_load(&root->files) + atomic_load(&root->dirs)
                            : atomic_load(&root->bytes);
        totalFilesCount += atomic_load(&root->files);
        if (atomic_load(&root->time) > latest) latest = atomic_load(&root->time);
    }
    if (opt.total) {
        printLead(stdout, &opt, total, totalFilesCount, latest);
        fputs("total", stdout);
        endLine(stdout, &opt);
    }
    fflush(stdout);
    return atomic_load(&failed) ? 1 : 0;
}
