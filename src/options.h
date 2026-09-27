/*
 * The command line both fastdus share, and how they print a size.
 */

#ifndef FASTDU_OPTIONS_H
#define FASTDU_OPTIONS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FASTDU_STRING2(x) #x
#define FASTDU_STRING(x) FASTDU_STRING2(x)
#ifdef FASTDU_VERSION
#define VERSION FASTDU_STRING(FASTDU_VERSION)
#else
#define VERSION "dev"
#endif

typedef struct {
    int depth; /* -1: any */
    long long minBytes;
    int threads; /* 0: one per logical processor */
    int human;
    int progress;
    char **roots;
    int rootCount;
} Options;

static const char USAGE[] =
    "usage: fastdu [-d DEPTH] [-m SIZE] [-j THREADS] [-h] [-p] [PATH...]\n"
    "\n"
    "Prints each folder's size, file count and path, tab-separated, for every\n"
    "folder under each PATH (default: the current folder).\n"
    "\n"
    "  -d DEPTH    only folders at most DEPTH below a PATH (0: the PATH alone)\n"
    "  -m SIZE     only folders of at least SIZE bytes; K, M, G, T are powers of 1024\n"
    "  -j THREADS  threads to walk with (default: one per logical processor)\n"
    "  -h          sizes as 4.0K, 12M, 1.5G\n"
    "  -p          \"progress<TAB>folders<TAB>files<TAB>bytes\" on standard error each second\n"
    "  --help      this text\n"
    "  --version   the version\n";

static void usageError(const char *what, const char *arg) {
    fprintf(stderr, "fastdu: %s%s\n\n%s", what, arg, USAGE);
    exit(2);
}

/* A size such as 512, 10K or 1.5G; -1 when it is not one. */
static long long parseSize(const char *text) {
    char *end;
    double value = strtod(text, &end);
    if (end == text || value < 0) return -1;
    double scale = 1;
    switch (*end) {
    case 'k': case 'K': scale = 1024.0; end++; break;
    case 'm': case 'M': scale = 1024.0 * 1024; end++; break;
    case 'g': case 'G': scale = 1024.0 * 1024 * 1024; end++; break;
    case 't': case 'T': scale = 1024.0 * 1024 * 1024 * 1024; end++; break;
    }
    if (*end != 0) return -1;
    return (long long)(value * scale);
}

/* A whole number at least `least`; -1 when it is not one. */
static int parseCount(const char *text, int least) {
    char *end;
    long value = strtol(text, &end, 10);
    if (end == text || *end != 0 || value < least || value > 1 << 20) return -1;
    return (int)value;
}

/* Exits after --help and --version, and on a mistake. */
static void parseOptions(int argc, char **argv, Options *options) {
    options->depth = -1;
    options->minBytes = 0;
    options->threads = 0;
    options->human = 0;
    options->progress = 0;
    options->roots = (char **)malloc((size_t)(argc + 1) * sizeof *options->roots);
    options->rootCount = 0;
    int onlyPaths = 0;
    for (int i = 1; i < argc; i++) {
        char *arg = argv[i];
        if (onlyPaths || arg[0] != '-' || arg[1] == 0) {
            options->roots[options->rootCount++] = arg;
            continue;
        }
        if (strcmp(arg, "--") == 0) {
            onlyPaths = 1;
            continue;
        }
        if (strcmp(arg, "--help") == 0) {
            fputs(USAGE, stdout);
            exit(0);
        }
        if (strcmp(arg, "--version") == 0) {
            printf("fastdu %s\n", VERSION);
            exit(0);
        }
        if (arg[1] == '-') usageError("unknown option ", arg);
        /* Flags may be bundled (-hp), and a value may follow its flag (-d1) or come next. */
        for (const char *flag = arg + 1; *flag != 0; flag++) {
            if (*flag == 'h') {
                options->human = 1;
                continue;
            }
            if (*flag == 'p') {
                options->progress = 1;
                continue;
            }
            if (*flag != 'd' && *flag != 'm' && *flag != 'j') {
                char unknown[3] = {'-', *flag, 0};
                usageError("unknown option ", unknown);
            }
            const char *value = flag[1] != 0 ? flag + 1 : i + 1 < argc ? argv[++i] : NULL;
            char name[3] = {'-', *flag, 0};
            if (value == NULL) usageError("a value must follow ", name);
            if (*flag == 'd' && (options->depth = parseCount(value, 0)) < 0)
                usageError("not a depth: ", value);
            if (*flag == 'm' && (options->minBytes = parseSize(value)) < 0)
                usageError("not a size: ", value);
            if (*flag == 'j' && (options->threads = parseCount(value, 1)) < 0)
                usageError("not a thread count: ", value);
            break;
        }
    }
    if (options->rootCount == 0) options->roots[options->rootCount++] = (char *)".";
}

static void printSize(FILE *out, long long bytes, int human) {
    if (!human) {
        fprintf(out, "%lld", bytes);
        return;
    }
    const char *units = "BKMGTPE";
    double value = (double)bytes;
    int unit = 0;
    while (value >= 1024 && unit < 6) {
        value /= 1024;
        unit++;
    }
    if (unit == 0) fprintf(out, "%lld", bytes);
    else if (value < 10) fprintf(out, "%.1f%c", value, units[unit]);
    else fprintf(out, "%.0f%c", value, units[unit]);
}

#endif
