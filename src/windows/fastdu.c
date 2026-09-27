/*
 * fastdu: folder sizes on Windows drives, from the directory listings alone.
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
 * Sizes are file lengths, as Explorer shows them. Junctions and symbolic links
 * are not followed; hard links are counted at each name. "C:" means the
 * drive's root. The command line is in `options.h` (`fastdu --help`).
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
            fprintf(stderr, "out of memory\n");
            ExitProcess(3);
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
    WCHAR *name; /* its own name; a root's is its whole path */
    USHORT nameLength; /* in bytes */
    int depth;
    volatile LONG64 bytes; /* its subtree, once complete */
    volatile LONG64 files;
    volatile LONG pending; /* subfolders not yet complete, plus its own listing */
} Node;

/* A folder given on the command line: its name as given, and how it is opened. */
typedef struct {
    Node node;
    WCHAR *openAs; /* its NT path */
    USHORT openLength; /* in bytes */
    volatile LONG failed;
} Root;

static int maxDepth;
static LONG64 minBytes;

/* Folders to report, no deeper than maxDepth: few, so one lock is enough. */
static SRWLOCK shownLock = SRWLOCK_INIT;
static Node **shown;
static size_t shownCount, shownCap;

static void show(Node *node) {
    AcquireSRWLockExclusive(&shownLock);
    if (shownCount == shownCap) {
        shownCap = shownCap ? shownCap * 2 : 1024;
        shown = (Node **)realloc(shown, shownCap * sizeof *shown);
    }
    shown[shownCount++] = node;
    ReleaseSRWLockExclusive(&shownLock);
}

/* A folder whose listing and every subfolder are done adds itself to its parent. */
static void complete(Node *node) {
    while (node != NULL) {
        if (InterlockedDecrement(&node->pending) != 0) return;
        Node *parent = node->parent;
        if (parent == NULL) return;
        InterlockedAdd64(&parent->bytes, node->bytes);
        InterlockedAdd64(&parent->files, node->files);
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
static WCHAR *fullPath(Node *node, Arena *arena, USHORT *length) {
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

static HANDLE openDir(HANDLE parent, WCHAR *name, USHORT length, NTSTATUS *result) {
    UNICODE_STRING path = {length, length, name};
    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, &path, OBJ_CASE_INSENSITIVE, parent, NULL);
    IO_STATUS_BLOCK status;
    HANDLE handle;
    *result = NtOpenFile(&handle, FILE_LIST_DIRECTORY | SYNCHRONIZE, &attributes, &status,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT |
                             FILE_OPEN_REPARSE_POINT);
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

/* Why a root could not be read, in Windows' words. */
static void complain(Root *root, NTSTATUS status) {
    WCHAR text[512];
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
                             RtlNtStatusToDosError(status), 0, text, 512, NULL);
    while (n > 0 && (text[n - 1] == L'\r' || text[n - 1] == L'\n' || text[n - 1] == L' ')) n--;
    char *name = utf8Of(root->node.name, root->node.nameLength / 2);
    char *why = n > 0 ? utf8Of(text, (int)n) : utf8Of(L"cannot be read", -1);
    fprintf(stderr, "fastdu: %s: %s\n", name, why);
}

#define BUFFER (256 * 1024)

typedef struct {
    Arena arena;
    BYTE *buffer;
} Worker;

static Node *newNode(Worker *self, Node *parent, const WCHAR *name, USHORT length) {
    Node *node = (Node *)take(&self->arena, sizeof(Node));
    node->parent = parent;
    node->name = (WCHAR *)take(&self->arena, length + sizeof(WCHAR));
    memcpy(node->name, name, length);
    node->name[length / 2] = 0;
    node->nameLength = length;
    node->depth = parent == NULL ? 0 : parent->depth + 1;
    node->bytes = 0;
    node->files = 0;
    node->pending = 1;
    if (maxDepth < 0 || node->depth <= maxDepth) show(node);
    return node;
}

/*
 * List one open folder, then walk its subfolders depth-first from this
 * handle — except those given away to idle threads.
 */
static void walk(Worker *self, Node *node, HANDLE handle) {
    Node **children = NULL;
    size_t count = 0, cap = 0;
    LONG64 bytes = 0, files = 0;
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
        if (result < 0 || result == STATUS_NO_MORE_FILES_) break;
        DIR_INFO *entry = (DIR_INFO *)self->buffer;
        for (;;) {
            if (entry->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                USHORT length = (USHORT)entry->FileNameLength;
                BOOL dot = (length == 2 && entry->FileName[0] == L'.') ||
                           (length == 4 && entry->FileName[0] == L'.' && entry->FileName[1] == L'.');
                if (!dot && !(entry->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                    if (count == cap) {
                        size_t grown = cap ? cap * 2 : 16;
                        Node **more = (Node **)take(&self->arena, grown * sizeof *more);
                        if (count) memcpy(more, children, count * sizeof *more);
                        children = more;
                        cap = grown;
                    }
                    children[count++] = newNode(self, node, entry->FileName, length);
                }
            } else {
                bytes += entry->EndOfFile.QuadPart;
                files += 1;
            }
            if (entry->NextEntryOffset == 0) break;
            entry = (DIR_INFO *)((BYTE *)entry + entry->NextEntryOffset);
        }
    }
    InterlockedAdd64(&node->bytes, bytes);
    InterlockedAdd64(&node->files, files);
    InterlockedIncrement64(&totalDirs);
    InterlockedAdd64(&totalFiles, files);
    InterlockedAdd64(&totalBytes, bytes);
    /* Each child is one more thing this folder waits for; its own listing is done. */
    InterlockedAdd(&node->pending, (LONG)count);
    for (size_t i = 0; i < count; i++) {
        Node *child = children[i];
        if (idle > 0 && i + 1 < count) {
            give(child);
            continue;
        }
        NTSTATUS result;
        HANDLE sub = openDir(handle, child->name, child->nameLength, &result);
        if (sub == NULL) {
            complete(child); /* unreadable: counted as empty */
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
        NTSTATUS result = 0;
        if (node->parent == NULL) {
            Root *root = (Root *)node;
            handle = openDir(NULL, root->openAs, root->openLength, &result);
            if (handle == NULL) {
                root->failed = 1;
                complain(root, result);
            }
        } else {
            USHORT length;
            WCHAR *path = fullPath(node, &self.arena, &length);
            if (path != NULL) handle = openDir(NULL, path, length, &result);
        }
        if (handle == NULL) {
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

/* The path as printed: the root as given, then each name. */
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

int wmain(int argc, WCHAR **wideArgv) {
    char **argv = (char **)malloc((size_t)argc * sizeof *argv);
    for (int i = 0; i < argc; i++) argv[i] = utf8Of(wideArgv[i], -1);
    Options options;
    parseOptions(argc, argv, &options);
    maxDepth = options.depth;
    minBytes = options.minBytes;
    threads = options.threads > 0 ? options.threads
                                  : (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (threads < 1) threads = 1;
    Arena main = {NULL, 0};
    Root **roots = (Root **)malloc((size_t)options.rootCount * sizeof *roots);
    for (int i = 0; i < options.rootCount; i++) {
        WCHAR *given = wideOf(options.roots[i]);
        size_t length = wcslen(given);
        while (length > 2 && given[length - 1] == L'\\') length--;
        Root *root = (Root *)take(&main, sizeof(Root));
        root->node.parent = NULL;
        root->node.name = given;
        root->node.nameLength = (USHORT)(length * sizeof(WCHAR));
        root->node.depth = 0;
        root->node.bytes = 0;
        root->node.files = 0;
        root->node.pending = 1;
        root->openAs = ntPathOf(given, &root->openLength);
        root->failed = 0;
        roots[i] = root;
        show(&root->node);
        give(&root->node);
    }
    if (options.progress) CreateThread(NULL, 0, reporter, NULL, 0, NULL);
    HANDLE *pool = (HANDLE *)malloc((size_t)threads * sizeof *pool);
    for (int i = 0; i < threads; i++) pool[i] = CreateThread(NULL, 1 << 20, worker, NULL, 0, NULL);
    WaitForMultipleObjects((DWORD)threads, pool, TRUE, INFINITE);

    /* Paths are printed in UTF-8, which a console shows only in its UTF-8 code page. */
    UINT codePage = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
    static char out[1 << 20];
    setvbuf(stdout, out, _IOFBF, sizeof out);
    for (size_t i = 0; i < shownCount; i++) {
        Node *node = shown[i];
        if (node->parent == NULL && ((Root *)node)->failed) continue;
        if (node->bytes < minBytes) continue;
        printSize(stdout, node->bytes, options.human);
        fprintf(stdout, "\t%lld\t", node->files);
        printPath(stdout, node);
        fputc('\n', stdout);
    }
    fflush(stdout);
    if (codePage != 0) SetConsoleOutputCP(codePage);
    int failed = 0;
    for (int i = 0; i < options.rootCount; i++) failed |= roots[i]->failed;
    return failed;
}
