/*
 * fastdu for Windows: GNU du's command line, on every core, from the directory
 * listings alone.
 *
 * The C version of fastdu.cs, for speed. Where the C# one spent its time:
 *   - opening each of the 560,000 folders on C: by its full path, which the
 *     kernel parses component by component. Here a folder is opened relative
 *     to its parent's open handle (NtOpenFile with RootDirectory), so only its
 *     own name is looked up;
 *   - a managed string per file name and one shared garbage-collected heap
 *     for all threads. Here entries are read in place from the kernel's
 *     buffer; only folder names are copied, into per-thread arenas;
 *   - FindNextFile's small batches. Here NtQueryDirectoryFile fills a
 *     256 KB buffer per call.
 * A thread walks its own subtree depth-first, keeping parent handles open, and
 * hands folders to a shared stack only while another thread is idle.
 *
 * Every size and time comes with the name in the listing: disk usage is the
 * allocation size, --apparent-size the file length (what Explorer shows). What
 * differs from du, because the listing cannot tell: hard links count at each
 * name (-l changes nothing), junctions and symbolic links are entries of their
 * own and never followed (but a path given, with -D or -H), -L is refused, and
 * nothing mounted in a folder is walked into (-x changes nothing). "C:" means
 * the drive's root. The command line is in `options.h`.
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _CRT_SECURE_NO_WARNINGS /* every copy is sized beforehand */
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../options.h"
#include "../output.h"

#pragma comment(lib, "ntdll.lib")

/* ---- NT declarations winternl.h leaves out ---- */

typedef struct {
    ULONG NextEntryOffset;
    ULONG FileIndex;
    LARGE_INTEGER CreationTime, LastAccessTime, LastWriteTime, ChangeTime;
    LARGE_INTEGER EndOfFile;
    LARGE_INTEGER AllocationSize;
    ULONG FileAttributes;
    ULONG FileNameLength;
    WCHAR FileName[1];
} DIR_INFO;

#define FileDirectoryInformationClass 1
#define STATUS_NO_MORE_FILES_ ((NTSTATUS)0x80000006L)

NTSYSCALLAPI NTSTATUS NTAPI NtQueryDirectoryFile(HANDLE, HANDLE, PVOID, PVOID, PIO_STATUS_BLOCK,
                                                 PVOID, ULONG, ULONG, BOOLEAN, PUNICODE_STRING,
                                                 BOOLEAN);

static Options opt;
static int pathsForExcludes;
static volatile LONG failed;

/* ---- per-thread memory: bump allocation, never freed before exit ---- */

typedef struct {
    char *at;
    size_t left;
} Arena;

static void *take(Arena *arena, size_t size) {
    size = (size + 15) & ~(size_t)15;
    if (arena->left < size) {
        size_t chunk = size > (4u << 20) ? size : (4u << 20);
        arena->at = (char *)malloc(chunk);
        if (arena->at == NULL) {
            fputs("fastdu: memory exhausted\n", stderr);
            ExitProcess(1);
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
    WCHAR *name; /* its own name; a root's is its path as given */
    USHORT nameLength; /* in bytes */
    int depth;
    unsigned char isDir, printed;
    LONG64 ownBytes, ownFiles, ownTime; /* itself and its files, for -S */
    volatile LONG64 bytes, files, dirs, time; /* its subtree, once complete */
    volatile LONG pending; /* subfolders not yet complete, plus its own listing */
} Node;

/* A path given on the command line: its name as given, and how it is opened. */
typedef struct {
    Node node;
    WCHAR *openAs; /* its NT path */
    USHORT openLength; /* in bytes */
} Root;

static SRWLOCK shownLock = SRWLOCK_INIT;
static Node **shown;
static size_t shownCount, shownCap;

static int shownAt(int depth) { return opt.depth < 0 || depth <= opt.depth; }

static void show(Node *node) {
    node->printed = 1;
    AcquireSRWLockExclusive(&shownLock);
    if (shownCount == shownCap) {
        shownCap = shownCap ? shownCap * 2 : 1024;
        shown = (Node **)realloc(shown, shownCap * sizeof *shown);
    }
    shown[shownCount++] = node;
    ReleaseSRWLockExclusive(&shownLock);
}

static void timeMax(volatile LONG64 *into, LONG64 value) {
    LONG64 seen = *into;
    while (value > seen) {
        LONG64 was = InterlockedCompareExchange64(into, value, seen);
        if (was == seen) return;
        seen = was;
    }
}

/* A folder whose listing and every subfolder are done adds itself to its parent. */
static void complete(Node *node) {
    while (node != NULL) {
        if (InterlockedDecrement(&node->pending) != 0) return;
        Node *parent = node->parent;
        if (parent == NULL) return;
        InterlockedAdd64(&parent->bytes, node->bytes);
        InterlockedAdd64(&parent->files, node->files);
        InterlockedAdd64(&parent->dirs, node->dirs);
        timeMax(&parent->time, node->time);
        node = parent;
    }
}

/* ---- shared work: a stack of folders to open by full path ---- */

static SRWLOCK workLock = SRWLOCK_INIT;
static CONDITION_VARIABLE workReady = CONDITION_VARIABLE_INIT;
static Node **work;
static size_t workCount, workCap;
static volatile LONG idle;
static int threads;
static volatile LONG finished;

static volatile LONG64 totalDirs, totalFiles, totalBytes;

static void give(Node *node) {
    AcquireSRWLockExclusive(&workLock);
    if (workCount == workCap) {
        workCap = workCap ? workCap * 2 : 4096;
        work = (Node **)realloc(work, workCap * sizeof *work);
    }
    work[workCount++] = node;
    ReleaseSRWLockExclusive(&workLock);
    WakeConditionVariable(&workReady);
}

/*
 * The NT path a root is opened by: "\??\" and its full Win32 path, or
 * "\??\UNC\" and a share's. "C:" alone is the drive's root, not the folder the
 * shell is in on it, and a drive's root keeps its backslash: "\??\C:" is the
 * volume itself.
 */
static WCHAR *ntPathOf(const WCHAR *given, USHORT *length) {
    WCHAR drive[4] = {given[0], L':', L'\\', 0};
    const WCHAR *path = wcslen(given) == 2 && given[1] == L':' ? drive : given;
    DWORD need = GetFullPathNameW(path, 0, NULL, NULL);
    WCHAR *full = (WCHAR *)malloc((need + 1) * sizeof(WCHAR));
    DWORD got = GetFullPathNameW(path, need + 1, full, NULL);
    full[got] = 0;
    WCHAR *out = (WCHAR *)malloc((got + 16) * sizeof(WCHAR));
    if (wcsncmp(full, L"\\\\?\\", 4) == 0 || wcsncmp(full, L"\\\\.\\", 4) == 0) {
        wcscpy(out, L"\\??\\");
        wcscat(out, full + 4);
    } else if (wcsncmp(full, L"\\\\", 2) == 0) {
        wcscpy(out, L"\\??\\UNC\\");
        wcscat(out, full + 2);
    } else {
        wcscpy(out, L"\\??\\");
        wcscat(out, full);
    }
    free(full);
    size_t used = wcslen(out);
    while (used > 4 && out[used - 1] == L'\\' && out[used - 2] != L':') used--;
    out[used] = 0;
    *length = (USHORT)(used * sizeof(WCHAR));
    return out;
}

/*
 * The NT path of a folder given away by its parent's thread: its root's, then
 * "\" and each name down to it. NULL when it is too deep to follow up.
 */
static WCHAR *ntPath(Node *node, Arena *arena, USHORT *length) {
    Node *chain[4096];
    int depth = 0;
    size_t chars = 0;
    Node *at = node;
    for (; at->parent != NULL; at = at->parent) {
        if (depth == 4096) return NULL;
        chain[depth++] = at;
        chars += at->nameLength / 2 + 1;
    }
    Root *root = (Root *)at;
    chars += root->openLength / 2;
    WCHAR *out = (WCHAR *)take(arena, (chars + 1) * sizeof(WCHAR));
    memcpy(out, root->openAs, root->openLength);
    size_t used = root->openLength / 2;
    for (int i = depth - 1; i >= 0; i--) {
        if (out[used - 1] != L'\\') out[used++] = L'\\';
        memcpy(out + used, chain[i]->name, chain[i]->nameLength);
        used += chain[i]->nameLength / 2;
    }
    out[used] = 0;
    *length = (USHORT)(used * sizeof(WCHAR));
    return out;
}

static HANDLE openDir(HANDLE parent, WCHAR *name, USHORT length, int follow, NTSTATUS *result) {
    UNICODE_STRING path = {length, length, name};
    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, &path, OBJ_CASE_INSENSITIVE, parent, NULL);
    IO_STATUS_BLOCK status;
    HANDLE handle;
    ULONG options = FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT;
    if (!follow) options |= FILE_OPEN_REPARSE_POINT;
    *result = NtOpenFile(&handle, FILE_LIST_DIRECTORY | SYNCHRONIZE, &attributes, &status,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, options);
    return *result < 0 ? NULL : handle;
}

static char *utf8Of(const WCHAR *text, int length) {
    int size = WideCharToMultiByte(CP_UTF8, 0, text, length, NULL, 0, NULL, NULL);
    char *out = (char *)malloc((size_t)size + 1);
    WideCharToMultiByte(CP_UTF8, 0, text, length, out, size, NULL, NULL);
    out[size] = 0;
    return out;
}

static WCHAR *wideOf(const char *text) {
    int size = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    WCHAR *out = (WCHAR *)malloc((size_t)size * sizeof(WCHAR));
    MultiByteToWideChar(CP_UTF8, 0, text, -1, out, size);
    return out;
}

/* A node's path as printed, in UTF-8: its root as given, then each name. */
static char *printedPath(Node *node) {
    Node *chain[4096];
    int depth = 0;
    size_t chars = 0;
    for (Node *at = node; at != NULL && depth < 4096; at = at->parent) {
        chain[depth++] = at;
        chars += at->nameLength / 2 + 1;
    }
    WCHAR *wide = (WCHAR *)malloc((chars + 1) * sizeof(WCHAR));
    size_t used = 0;
    for (int i = depth - 1; i >= 0; i--) {
        if (i != depth - 1 && used > 0 && wide[used - 1] != L'\\') wide[used++] = L'\\';
        memcpy(wide + used, chain[i]->name, chain[i]->nameLength);
        used += chain[i]->nameLength / 2;
    }
    char *out = utf8Of(wide, (int)used);
    free(wide);
    return out;
}

/* Why something could not be read, in Windows' words, as du says it. */
static void complain(const char *what, const char *path, DWORD error) {
    WCHAR text[512];
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
                             error, 0, text, 512, NULL);
    while (n > 0 && (text[n - 1] == L'\r' || text[n - 1] == L'\n' || text[n - 1] == L' ')) n--;
    char *why = n > 0 ? utf8Of(text, (int)n) : utf8Of(L"cannot be read", -1);
    fprintf(stderr, "fastdu: %s '%s': %s\n", what, path, why);
    free(why);
    InterlockedExchange(&failed, 1);
}

/* ---- what the listing says, as du counts it ---- */

/* FILETIME ticks (100 ns since 1601) as nanoseconds since 1970. */
static LONG64 nanosOf(LARGE_INTEGER ticks) {
    return (ticks.QuadPart - 116444736000000000LL) * 100;
}

static LONG64 sizeOfEntry(const DIR_INFO *entry) {
    return opt.apparent ? entry->EndOfFile.QuadPart : entry->AllocationSize.QuadPart;
}

static LONG64 timeOfEntry(const DIR_INFO *entry) {
    if (opt.time == TIME_ACCESSED) return nanosOf(entry->LastAccessTime);
    if (opt.time == TIME_CHANGED) return nanosOf(entry->ChangeTime);
    return nanosOf(entry->LastWriteTime);
}

#define BUFFER (256 * 1024)

typedef struct {
    Arena arena;
    BYTE *buffer;
} Worker;

static Node *newNode(Arena *arena, Node *parent, const WCHAR *name, USHORT length, int isDir) {
    Node *node = (Node *)take(arena, sizeof(Node));
    memset(node, 0, sizeof *node);
    node->parent = parent;
    node->name = (WCHAR *)take(arena, length + sizeof(WCHAR));
    memcpy(node->name, name, length);
    node->name[length / 2] = 0;
    node->nameLength = length;
    node->depth = parent == NULL ? 0 : parent->depth + 1;
    node->isDir = (unsigned char)isDir;
    node->pending = 1;
    return node;
}

/* Whether --exclude or -X leaves this entry of `parent` out. */
static int excludedChild(Node *parent, const WCHAR *name, USHORT length) {
    if (opt.excludeCount == 0) return 0;
    char *own = utf8Of(name, length / 2);
    int out;
    if (!pathsForExcludes) {
        out = excluded(&opt, own);
    } else {
        char *above = printedPath(parent);
        size_t a = strlen(above), n = strlen(own);
        char *path = (char *)malloc(a + n + 2);
        memcpy(path, above, a);
        size_t used = a;
        if (used > 0 && path[used - 1] != '\\') path[used++] = '\\';
        memcpy(path + used, own, n + 1);
        out = excluded(&opt, path);
        free(path);
        free(above);
    }
    free(own);
    return out;
}

/* A folder counted as itself only: it could not be opened, or read. */
static void folderAlone(Node *node) {
    node->bytes = node->ownBytes;
    node->dirs = 1;
    node->time = node->ownTime;
    if (shownAt(node->depth)) show(node);
}

/*
 * List one open folder, then walk its subfolders depth-first from this
 * handle — except those given away to idle threads.
 */
static void walk(Worker *self, Node *node, HANDLE handle) {
    Node **children = NULL;
    size_t count = 0, cap = 0;
    LONG64 bytes = 0, files = 0, latest = node->ownTime;
    IO_STATUS_BLOCK status;
    BOOLEAN restart = TRUE;
    for (;;) {
        NTSTATUS result = NtQueryDirectoryFile(handle, NULL, NULL, NULL, &status, self->buffer,
                                               BUFFER, FileDirectoryInformationClass, FALSE, NULL,
                                               restart);
        restart = FALSE;
        /*
         * Only "no more files" ends a listing. A batch that leaves room in the
         * buffer is not the end: NTFS returns short batches mid-folder, and
         * stopping at one counted 1,171,304 files on C: where there were
         * 1,856,491 (2026-09-27).
         */
        if (result == STATUS_NO_MORE_FILES_) break;
        if (result < 0) {
            char *path = printedPath(node);
            complain("cannot read directory", path, RtlNtStatusToDosError(result));
            free(path);
            break;
        }
        DIR_INFO *entry = (DIR_INFO *)self->buffer;
        for (;;) {
            USHORT length = (USHORT)entry->FileNameLength;
            BOOL dot = (length == 2 && entry->FileName[0] == L'.') ||
                       (length == 4 && entry->FileName[0] == L'.' && entry->FileName[1] == L'.');
            if (!dot && !excludedChild(node, entry->FileName, length)) {
                LONG64 size = sizeOfEntry(entry), when = timeOfEntry(entry);
                int folder = (entry->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                             !(entry->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
                if (folder) {
                    if (count == cap) {
                        size_t grown = cap ? cap * 2 : 16;
                        Node **more = (Node **)take(&self->arena, grown * sizeof *more);
                        if (count) memcpy(more, children, count * sizeof *more);
                        children = more;
                        cap = grown;
                    }
                    Node *child = newNode(&self->arena, node, entry->FileName, length, 1);
                    child->ownBytes = size;
                    child->ownTime = when;
                    children[count++] = child;
                } else {
                    /* A file, or a junction or symbolic link, counted as itself. */
                    bytes += size;
                    files += 1;
                    if (when > latest) latest = when;
                    if (opt.all && shownAt(node->depth + 1)) {
                        Node *file = newNode(&self->arena, node, entry->FileName, length, 0);
                        file->ownBytes = file->bytes = size;
                        file->ownFiles = file->files = 1;
                        file->ownTime = file->time = when;
                        file->pending = 0;
                        show(file);
                    }
                }
            }
            if (entry->NextEntryOffset == 0) break;
            entry = (DIR_INFO *)((BYTE *)entry + entry->NextEntryOffset);
        }
    }
    node->ownBytes += bytes;
    node->ownFiles = files;
    node->ownTime = latest;
    InterlockedAdd64(&node->bytes, node->ownBytes);
    InterlockedAdd64(&node->files, files);
    InterlockedAdd64(&node->dirs, 1);
    timeMax(&node->time, latest);
    InterlockedIncrement64(&totalDirs);
    InterlockedAdd64(&totalFiles, files);
    InterlockedAdd64(&totalBytes, node->ownBytes);
    if (shownAt(node->depth)) show(node);
    /* Each child is one more thing this folder waits for; its own listing is done. */
    InterlockedAdd(&node->pending, (LONG)count);
    for (size_t i = 0; i < count; i++) {
        Node *child = children[i];
        if (idle > 0 && i + 1 < count) {
            give(child);
            continue;
        }
        NTSTATUS result;
        HANDLE sub = openDir(handle, child->name, child->nameLength, 0, &result);
        if (sub == NULL) {
            char *path = printedPath(child);
            complain("cannot read directory", path, RtlNtStatusToDosError(result));
            free(path);
            folderAlone(child);
            complete(child);
            continue;
        }
        walk(self, child, sub);
    }
    CloseHandle(handle);
    complete(node);
}

static DWORD WINAPI worker(LPVOID unused) {
    (void)unused;
    Worker self = {{NULL, 0}, (BYTE *)VirtualAlloc(NULL, BUFFER, MEM_COMMIT, PAGE_READWRITE)};
    for (;;) {
        AcquireSRWLockExclusive(&workLock);
        InterlockedIncrement(&idle);
        while (workCount == 0) {
            if (idle == threads) {
                /* Nothing queued and nobody walking: the walk is over. */
                finished = 1;
                ReleaseSRWLockExclusive(&workLock);
                WakeAllConditionVariable(&workReady);
                return 0;
            }
            if (finished) {
                ReleaseSRWLockExclusive(&workLock);
                return 0;
            }
            SleepConditionVariableSRW(&workReady, &workLock, INFINITE, 0);
        }
        Node *node = work[--workCount];
        InterlockedDecrement(&idle);
        ReleaseSRWLockExclusive(&workLock);
        HANDLE handle = NULL;
        NTSTATUS result = (NTSTATUS)0xC0000106L; /* name too long */
        if (node->parent == NULL) {
            Root *root = (Root *)node;
            handle = openDir(NULL, root->openAs, root->openLength, opt.derefArgs, &result);
        } else {
            USHORT length;
            WCHAR *path = ntPath(node, &self.arena, &length);
            if (path != NULL) handle = openDir(NULL, path, length, 0, &result);
        }
        if (handle == NULL) {
            char *path = printedPath(node);
            complain("cannot read directory", path, RtlNtStatusToDosError(result));
            free(path);
            folderAlone(node);
            complete(node);
            continue;
        }
        walk(&self, node, handle);
    }
}

static DWORD WINAPI reporter(LPVOID unused) {
    (void)unused;
    for (;;) {
        Sleep(1000);
        fprintf(stderr, "progress\t%lld\t%lld\t%lld\n", totalDirs, totalFiles, totalBytes);
        fflush(stderr);
    }
}

/* ---- printing, in du's order: a folder after everything in it ---- */

static void printPath(FILE *out, Node *node) {
    if (node->parent != NULL) {
        printPath(out, node->parent);
        Node *above = node->parent;
        if (above->name[above->nameLength / 2 - 1] != L'\\') fputc('\\', out);
    }
    static char utf8[3 * 32768 + 8];
    int n = WideCharToMultiByte(CP_UTF8, 0, node->name, node->nameLength / 2, utf8,
                                (int)sizeof utf8, NULL, NULL);
    fwrite(utf8, 1, (size_t)n, out);
}

static LONG64 amountOf(Node *node) {
    int own = opt.separateDirs && node->isDir;
    if (opt.inodes) return own ? node->ownFiles + 1 : node->files + node->dirs;
    return own ? node->ownBytes : node->bytes;
}

static void printNode(FILE *out, Node *node) {
    LONG64 amount = amountOf(node);
    if (!shownBy(&opt, amount)) return;
    int own = opt.separateDirs && node->isDir;
    printLead(out, &opt, amount, own ? node->ownFiles : node->files,
              own ? node->ownTime : node->time);
    printPath(out, node);
    endLine(out, &opt);
}

static void printTree(FILE *out, Node *root) {
    /* Iterative post-order: a node is printed once its children are. */
    size_t cap = 1024, depth = 1;
    Node **stack = (Node **)malloc(cap * sizeof *stack);
    char *entered = (char *)malloc(cap);
    stack[0] = root;
    entered[0] = 0;
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
                stack = (Node **)realloc(stack, cap * sizeof *stack);
                entered = (char *)realloc(entered, cap);
            }
            stack[depth] = child;
            entered[depth] = 0;
            depth++;
        }
    }
    free(stack);
    free(entered);
}

/*
 * A path given, as du looks at it before walking: what it is, its size and
 * time. A junction or symbolic link is itself, unless -D or -H.
 */
static int statRoot(const WCHAR *path, int follow, int *isDir, LONG64 *size, LONG64 *when) {
    DWORD flags = FILE_FLAG_BACKUP_SEMANTICS | (follow ? 0 : FILE_FLAG_OPEN_REPARSE_POINT);
    HANDLE file = CreateFileW(path, FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                              OPEN_EXISTING, flags, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    FILE_BASIC_INFO basic;
    FILE_STANDARD_INFO standard;
    BOOL ok = GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof basic) &&
              GetFileInformationByHandleEx(file, FileStandardInfo, &standard, sizeof standard);
    CloseHandle(file);
    if (!ok) return 0;
    *isDir = (basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
             (follow || !(basic.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT));
    *size = *isDir ? 0 : opt.apparent ? standard.EndOfFile.QuadPart : standard.AllocationSize.QuadPart;
    LARGE_INTEGER t = opt.time == TIME_ACCESSED  ? basic.LastAccessTime
                      : opt.time == TIME_CHANGED ? basic.ChangeTime
                                                 : basic.LastWriteTime;
    *when = nanosOf(t);
    return 1;
}

int wmain(int argc, WCHAR **wideArgv) {
    char **argv = (char **)malloc((size_t)argc * sizeof *argv);
    for (int i = 0; i < argc; i++) argv[i] = utf8Of(wideArgv[i], -1);
    parseOptions(argc, argv, &opt);
    if (opt.deref) {
        fputs("fastdu: -L (--dereference) is not supported on Windows: junction loops cannot\n"
              "be told from the listing\n",
              stderr);
        return 1;
    }
    pathsForExcludes = excludesNeedPaths(&opt);
    threads = opt.threads > 0 ? opt.threads : (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (threads < 1) threads = 1;
    Arena main = {NULL, 0};
    Root **roots = (Root **)malloc((size_t)opt.rootCount * sizeof *roots);
    int rootCount = 0;
    for (int i = 0; i < opt.rootCount; i++) {
        WCHAR *given = wideOf(opt.roots[i]);
        Root *root = (Root *)take(&main, sizeof(Root));
        memset(root, 0, sizeof *root);
        root->node.name = given;
        root->node.nameLength = (USHORT)(wcslen(given) * sizeof(WCHAR));
        root->node.pending = 1;
        root->openAs = ntPathOf(given, &root->openLength);
        int isDir;
        LONG64 size, when;
        /* \??\ paths are NT paths; CreateFileW takes the \\?\ spelling of them. */
        WCHAR *win32 = (WCHAR *)malloc(root->openLength + sizeof(WCHAR));
        memcpy(win32, root->openAs, root->openLength);
        win32[root->openLength / 2] = 0;
        win32[1] = L'\\';
        if (!statRoot(win32, opt.derefArgs, &isDir, &size, &when)) {
            complain("cannot access", opt.roots[i], GetLastError());
            free(win32);
            continue;
        }
        free(win32);
        root->node.isDir = (unsigned char)isDir;
        root->node.ownBytes = size;
        root->node.ownTime = when;
        roots[rootCount++] = root;
        if (!isDir) {
            root->node.pending = 0;
            root->node.ownFiles = 1;
            root->node.bytes = size;
            root->node.files = 1;
            root->node.time = when;
            show(&root->node);
            continue;
        }
        give(&root->node);
    }
    if (opt.progress) CreateThread(NULL, 0, reporter, NULL, 0, NULL);
    HANDLE *pool = (HANDLE *)malloc((size_t)threads * sizeof *pool);
    for (int i = 0; i < threads; i++) pool[i] = CreateThread(NULL, 1 << 20, worker, NULL, 0, NULL);
    for (int at = 0; at < threads; at += MAXIMUM_WAIT_OBJECTS) {
        int count = threads - at < MAXIMUM_WAIT_OBJECTS ? threads - at : MAXIMUM_WAIT_OBJECTS;
        WaitForMultipleObjects((DWORD)count, pool + at, TRUE, INFINITE);
    }

    /* Link up the printed tree: each shown node under its parent. */
    for (size_t i = shownCount; i-- > 0;) {
        Node *node = shown[i];
        if (node->parent == NULL) continue;
        node->sibling = node->parent->child;
        node->parent->child = node;
    }
    /* Paths are printed in UTF-8, which a console shows only in its UTF-8 code page. */
    UINT codePage = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
    static char out[1 << 22];
    setvbuf(stdout, out, _IOFBF, sizeof out);
    LONG64 total = 0, totalFilesCount = 0, latest = 0;
    for (int i = 0; i < rootCount; i++) {
        Node *root = &roots[i]->node;
        if (!root->printed) continue;
        printTree(stdout, root);
        total += opt.inodes ? root->files + root->dirs : root->bytes;
        totalFilesCount += root->files;
        if (root->time > latest) latest = root->time;
    }
    if (opt.total) {
        printLead(stdout, &opt, total, totalFilesCount, latest);
        fputs("total", stdout);
        endLine(stdout, &opt);
    }
    fflush(stdout);
    if (codePage != 0) SetConsoleOutputCP(codePage);
    return failed ? 1 : 0;
}
