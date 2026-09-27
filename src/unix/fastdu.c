/*
 * fastdu for Linux and macOS: folder sizes, like `du -x`, on every core.
 *
 * The walk is the same on both; only listing a folder differs (`list`):
 *   - a Linux listing (getdents64) has names, inode numbers and types but no
 *     sizes, so files need a stat, each with fstatat relative to its folder's
 *     descriptor so the kernel resolves one name, not a whole path;
 *   - a macOS listing (getattrlistbulk) can carry each file's allocated size
 *     and link count with its name, as Windows' does, so nothing is stat-ed.
 * Around that:
 *   - a thread walks its own subtree depth-first and hands folders to a shared
 *     stack only while another thread is idle;
 *   - nothing is allocated per file; folder names go into per-thread arenas.
 * Counted like `du -x`: allocated blocks, one file system, a file with several
 * hard links once (a sharded set of inode numbers).
 *
 * The command line is in `options.h` (`fastdu --help`).
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
#include <unistd.h>

#if defined(__linux__)
#include <sys/syscall.h>
#elif defined(__APPLE__)
#include <sys/attr.h>
#include <sys/vnode.h>
#else
#error "fastdu is for Linux, macOS and Windows"
#endif

#include "../options.h"

#if defined(__linux__)
struct linux_dirent64 {
    uint64_t d_ino;
    int64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
};
#endif

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
            fputs("out of memory\n", stderr);
            exit(3);
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
    char *name; /* its own name; a root's is its whole path */
    int depth;
    dev_t device; /* its root's file system: nothing mounted inside is counted */
    atomic_llong bytes; /* its subtree, once complete */
    atomic_llong files;
    atomic_int pending; /* subfolders not yet complete, plus its own listing */
} Node;

static int maxDepth;
static long long minBytes;

static pthread_mutex_t shownLock = PTHREAD_MUTEX_INITIALIZER;
static Node **shown;
static size_t shownCount, shownCap;

static void show(Node *node) {
    pthread_mutex_lock(&shownLock);
    if (shownCount == shownCap) {
        shownCap = shownCap ? shownCap * 2 : 4096;
        shown = realloc(shown, shownCap * sizeof *shown);
    }
    shown[shownCount++] = node;
    pthread_mutex_unlock(&shownLock);
}

static void complete(Node *node) {
    while (node != NULL) {
        if (atomic_fetch_sub(&node->pending, 1) != 1) return;
        Node *parent = node->parent;
        if (parent == NULL) return;
        atomic_fetch_add(&parent->bytes, atomic_load(&node->bytes));
        atomic_fetch_add(&parent->files, atomic_load(&node->files));
        node = parent;
    }
}

/*
 * ---- every file seen, by file system and inode: each counted once ----
 * A file with several names (hard links) is counted at the first name met,
 * as du does. Every file goes in, not only those known to have several
 * names: the check comes before the stat, which is what it saves.
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

static Node *newNode(Arena *arena, Node *parent, const char *name, size_t length) {
    Node *node = take(arena, sizeof(Node));
    node->parent = parent;
    node->name = take(arena, length + 1);
    memcpy(node->name, name, length);
    node->name[length] = 0;
    node->depth = parent == NULL ? 0 : parent->depth + 1;
    node->device = parent == NULL ? 0 : parent->device;
    atomic_init(&node->bytes, 0);
    atomic_init(&node->files, 0);
    atomic_init(&node->pending, 1);
    return node;
}

/* The path of a folder given away by its parent's thread. */
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
        memcpy(out + used, chain[i]->name, n);
        used += n;
        if (i != 0) out[used++] = '/';
    }
    out[used] = 0;
    return out;
}

/* A folder's subfolders, found while listing it. */
typedef struct {
    Node **items;
    size_t count, cap;
} Children;

static void addChild(Worker *self, Node *node, const char *name, Children *children) {
    if (children->count == children->cap) {
        size_t grown = children->cap ? children->cap * 2 : 16;
        Node **more = take(&self->arena, grown * sizeof *more);
        if (children->count) memcpy(more, children->items, children->count * sizeof *more);
        children->items = more;
        children->cap = grown;
    }
    children->items[children->count++] = newNode(&self->arena, node, name, strlen(name));
}

#if defined(__linux__)
/*
 * List one folder: its subfolders, and its files' blocks and count.
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
static void list(Worker *self, Node *node, int fd, Children *children, long long *bytes,
                 long long *files) {
    for (;;) {
        long got = syscall(SYS_getdents64, fd, self->buffer, BUFFER);
        if (got <= 0) break;
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
            if (entry->d_type == DT_DIR) {
                addChild(self, node, name, children);
                continue;
            }
            /* The file system does not say what it is: ask. */
            if (entry->d_type == DT_UNKNOWN) {
                struct stat kind;
                if (fstatat(fd, name, &kind, AT_SYMLINK_NOFOLLOW) != 0) continue;
                if (S_ISDIR(kind.st_mode)) {
                    addChild(self, node, name, children);
                    continue;
                }
            }
            if (!firstSight(node->device, entry->d_ino)) continue;
            struct stat info;
            if (fstatat(fd, name, &info, AT_SYMLINK_NOFOLLOW) != 0) continue;
            *bytes += (long long)info.st_blocks * 512;
            *files += 1;
        }
        if (whole) break;
    }
}
#elif defined(__APPLE__)
/*
 * List one folder: its subfolders, and its files' blocks and count.
 *
 * getattrlistbulk returns, with each name, what is asked of it: here its
 * kind, file ID, link count and allocated size. So no file is stat-ed, and
 * only a file with several names is looked up in the set of files seen. Each
 * entry holds only the attributes it returned, in a fixed order, 4-byte
 * aligned, so they are read one after another as `returned` says.
 */
static void list(Worker *self, Node *node, int fd, Children *children, long long *bytes,
                 long long *files) {
    struct attrlist request;
    memset(&request, 0, sizeof request);
    request.bitmapcount = ATTR_BIT_MAP_COUNT;
    request.commonattr = ATTR_CMN_RETURNED_ATTRS | ATTR_CMN_ERROR | ATTR_CMN_NAME |
                         ATTR_CMN_OBJTYPE | ATTR_CMN_FILEID;
    request.fileattr = ATTR_FILE_LINKCOUNT | ATTR_FILE_ALLOCSIZE;
    for (;;) {
        int got = getattrlistbulk(fd, &request, self->buffer, BUFFER, 0);
        if (got <= 0) break;
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
            off_t allocated = 0;
            if (returned.fileattr & ATTR_FILE_ALLOCSIZE) memcpy(&allocated, field, sizeof allocated);
            if (error != 0 || name == NULL) continue;
            if (type == VDIR) {
                addChild(self, node, name, children);
                continue;
            }
            if (links > 1 && !firstSight(node->device, id)) continue;
            *bytes += (long long)allocated;
            *files += 1;
        }
    }
}
#endif

static void walk(Worker *self, Node *node, int fd) {
    if (node->parent != NULL) {
        struct stat own;
        /* Another file system mounted here, or gone: not this one's space. */
        if (fstat(fd, &own) != 0 || own.st_dev != node->device) {
            close(fd);
            complete(node);
            return;
        }
        /* A folder's own blocks are counted in it, as du does. */
        atomic_fetch_add(&node->bytes, (long long)own.st_blocks * 512);
    }
    Children children = {NULL, 0, 0};
    long long bytes = 0, files = 0;
    list(self, node, fd, &children, &bytes, &files);
    size_t count = children.count;
    atomic_fetch_add(&node->bytes, bytes);
    atomic_fetch_add(&node->files, files);
    atomic_fetch_add(&totalDirs, 1);
    atomic_fetch_add(&totalFiles, files);
    atomic_fetch_add(&totalBytes, bytes);
    if (maxDepth < 0 || node->depth <= maxDepth) show(node);
    atomic_fetch_add(&node->pending, (int)count);
    for (size_t i = 0; i < count; i++) {
        Node *child = children.items[i];
        if (atomic_load(&idle) > 0 && i + 1 < count) {
            give(child);
            continue;
        }
        int sub = openat(fd, child->name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (sub < 0) {
            if (maxDepth < 0 || child->depth <= maxDepth) show(child);
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
        /* A root given as a symbolic link is followed; nothing below one is. */
        int follow = node->parent == NULL ? 0 : O_NOFOLLOW;
        int fd = open(path, O_RDONLY | O_DIRECTORY | follow | O_CLOEXEC);
        if (fd < 0) {
            if (maxDepth < 0 || node->depth <= maxDepth) show(node);
            complete(node);
            continue;
        }
        walk(&self, node, fd);
    }
}

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

static void printPath(FILE *out, Node *node) {
    if (node->parent != NULL) {
        printPath(out, node->parent);
        const char *above = node->parent->name;
        if (above[strlen(above) - 1] != '/') fputc('/', out);
    }
    fputs(node->name, out);
}

int main(int argc, char **argv) {
    Options options;
    parseOptions(argc, argv, &options);
    maxDepth = options.depth;
    minBytes = options.minBytes;
    threads = options.threads > 0 ? options.threads : (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (threads < 1) threads = 1;
    for (int i = 0; i < SHARDS; i++) pthread_mutex_init(&seen[i].lock, NULL);
    int failed = 0;
    Arena main = {NULL, 0};
    for (int i = 0; i < options.rootCount; i++) {
        char *root = options.roots[i];
        size_t length = strlen(root);
        while (length > 1 && root[length - 1] == '/') length--;
        struct stat info;
        if (stat(root, &info) != 0) {
            fprintf(stderr, "fastdu: %s: %s\n", root, strerror(errno));
            failed = 1;
            continue;
        }
        Node *node = newNode(&main, NULL, root, length);
        node->device = info.st_dev;
        atomic_init(&node->bytes, (long long)info.st_blocks * 512);
        /* A file given is printed as itself, as du does. */
        if (!S_ISDIR(info.st_mode)) {
            atomic_init(&node->files, 1);
            show(node);
            continue;
        }
        give(node);
    }
    if (options.progress) {
        pthread_t tick;
        pthread_create(&tick, NULL, reporter, NULL);
    }
    pthread_t *pool = malloc((size_t)threads * sizeof *pool);
    for (int i = 0; i < threads; i++) pthread_create(&pool[i], NULL, worker, NULL);
    for (int i = 0; i < threads; i++) pthread_join(pool[i], NULL);

    static char out[1 << 22];
    setvbuf(stdout, out, _IOFBF, sizeof out);
    for (size_t i = 0; i < shownCount; i++) {
        Node *node = shown[i];
        long long bytes = atomic_load(&node->bytes);
        if (bytes < minBytes) continue;
        printSize(stdout, bytes, options.human);
        fprintf(stdout, "\t%lld\t", atomic_load(&node->files));
        printPath(stdout, node);
        fputc('\n', stdout);
    }
    fflush(stdout);
    return failed;
}
