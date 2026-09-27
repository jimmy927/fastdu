/*
 * fastdu's command line: GNU du's, switch for switch, plus -j (threads), -p
 * (progress) and --file-count (a file-count column). Shared by both walkers.
 */

#ifndef FASTDU_OPTIONS_H
#define FASTDU_OPTIONS_H

#include <errno.h>
#include <locale.h>
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

typedef enum { TIME_NONE, TIME_MODIFIED, TIME_ACCESSED, TIME_CHANGED } TimeKind;

typedef struct {
    int depth; /* -d; -1: any */
    int summarize; /* -s */
    int all; /* -a */
    int apparent; /* --apparent-size, -b */
    long long blockSize; /* the unit sizes are printed in */
    char blockSuffix[8]; /* printed after each size: -BM gives "M" */
    int grouping; /* -B "'1": digits grouped the locale's way ("97 657") */
    int human; /* -h: 1024, --si: 1000, else 0 */
    int total; /* -c */
    int nul; /* -0 */
    int derefArgs; /* -D, -H */
    int deref; /* -L */
    int countLinks; /* -l */
    int separateDirs; /* -S */
    long long threshold; /* -t: at least this, or at most -this; 0: none */
    int inodes; /* --inodes */
    TimeKind time; /* --time[=WORD] */
    char timeFormat[256]; /* strftime, and %N for nanoseconds */
    int oneFileSystem; /* -x */
    char **excludes; /* --exclude, -X */
    int excludeCount, excludeCap;
    int threads; /* -j; 0: the default */
    int progress; /* -p */
    int files; /* --file-count */
    char **roots;
    int rootCount;
} Options;

/*
 * The name messages say, as GNU's say theirs: how the program was started, so
 * installed as du it reports as du, and the tests written for du can read it.
 */
static const char *programName = "fastdu";

/* A name read by --files0-from that could not be used: the exit status is 1. */
static int badNames;

static void setProgramName(const char *argv0) {
    static char name[256];
    const char *base = argv0;
    for (const char *at = argv0; *at != 0; at++) {
        if (*at == '/' || *at == '\\') base = at + 1;
    }
    size_t length = strlen(base);
    if (length >= sizeof name) length = sizeof name - 1;
    memcpy(name, base, length);
    name[length] = 0;
    if (length > 4) {
        char *dot = name + length - 4;
        if (dot[0] == '.' && (dot[1] | 32) == 'e' && (dot[2] | 32) == 'x' && (dot[3] | 32) == 'e')
            *dot = 0;
    }
    if (name[0] != 0) programName = name;
}

static const char USAGE[] =
    "Summarize disk usage of the set of FILEs, recursively for directories, on\n"
    "every core. The options are GNU du's.\n"
    "\n"
    "  -0, --null            end each output line with NUL, not newline\n"
    "  -a, --all             write counts for all files, not just directories\n"
    "  -A, --apparent-size   print apparent sizes, rather than disk usage\n"
    "  -B, --block-size=SIZE  scale sizes by SIZE before printing them; e.g.,\n"
    "                           '-BM' prints sizes in units of 1,048,576 bytes\n"
    "  -b, --bytes           equivalent to '--apparent-size --block-size=1'\n"
    "  -c, --total           produce a grand total\n"
    "  -D, --dereference-args  dereference only symlinks that are listed on the\n"
    "                          command line\n"
    "  -d, --max-depth=N     print the total for a directory (or file, with --all)\n"
    "                          only if it is N or fewer levels below the command\n"
    "                          line argument;  --max-depth=0 is the same as\n"
    "                          --summarize\n"
    "      --files0-from=F   summarize disk usage of the NUL-terminated file\n"
    "                          names specified in file F; if F is -, then read\n"
    "                          names from standard input\n"
    "  -H                    equivalent to --dereference-args (-D)\n"
    "  -h, --human-readable  print sizes in human readable format (e.g., 1K 234M 2G)\n"
    "      --inodes          list inode usage information instead of block usage\n"
    "  -k                    like --block-size=1K\n"
    "  -L, --dereference     dereference all symbolic links\n"
    "  -l, --count-links     count sizes many times if hard linked\n"
    "  -m                    like --block-size=1M\n"
    "  -P, --no-dereference  don't follow any symbolic links (this is the default)\n"
    "  -S, --separate-dirs   for directories do not include size of subdirectories\n"
    "      --si              like -h, but use powers of 1000 not 1024\n"
    "  -s, --summarize       display only a total for each argument\n"
    "  -t, --threshold=SIZE  exclude entries smaller than SIZE if positive,\n"
    "                          or entries greater than SIZE if negative\n"
    "      --time            show time of the last modification of any file in the\n"
    "                          directory, or any of its subdirectories\n"
    "      --time=WORD       show time as WORD instead of modification time:\n"
    "                          atime, access, use, ctime or status\n"
    "      --time-style=STYLE  show times using STYLE, which can be:\n"
    "                            full-iso, long-iso, iso, or +FORMAT;\n"
    "                            FORMAT is interpreted like in 'date'\n"
    "  -X, --exclude-from=FILE  exclude files that match any pattern in FILE\n"
    "      --exclude=PATTERN    exclude files that match PATTERN\n"
    "  -x, --one-file-system    skip directories on different file systems\n"
    "\n"
    "fastdu's own (written out in full: they never shorten, so du's do as in du):\n"
    "  -j, --threads=N       walk with N threads (default: one per logical processor;\n"
    "                          on macOS more while they wait on the disk)\n"
    "  -p, --progress        'progress<TAB>folders<TAB>files<TAB>bytes' on standard\n"
    "                          error each second\n"
    "      --file-count      a column with the number of files, after the size\n"
    "      --help     display this help and exit\n"
    "      --version  output version information and exit\n"
    "\n"
    "Display values are in units of the first available SIZE from --block-size,\n"
    "and the DU_BLOCK_SIZE, BLOCK_SIZE and BLOCKSIZE environment variables.\n"
    "Otherwise, units default to 1024 bytes (or 512 if POSIXLY_CORRECT is set).\n"
    "\n"
    "The SIZE argument is an integer and optional unit (example: 10K is 10*1024).\n"
    "Units are K,M,G,T,P,E (powers of 1024) or KB,MB,... (powers of 1000).\n";

static void tryHelp(void) {
    fprintf(stderr, "Try '%s --help' for more information.\n", programName);
    exit(1);
}

static void failWith(const char *format, const char *arg) {
    fprintf(stderr, "%s: ", programName);
    fprintf(stderr, format, arg);
    fputc('\n', stderr);
    tryHelp();
}

/*
 * A size with an optional unit, as du takes them: "10", "10K", "M" alone
 * (-BM), "10KB" (powers of 1000), "10KiB". Returns 0 and the bytes, or -1.
 * `digits` says whether a number came before the unit; `suffix` gets the unit
 * as written.
 */
static int parseSize(const char *text, long long *out, int *digits, char suffix[8]) {
    const char *at = text;
    if (*at == '\'') at++; /* du's thousands grouping: nothing to group here */
    int negative = *at == '-';
    if (negative) at++;
    long long value = 1;
    *digits = *at >= '0' && *at <= '9';
    if (*digits) {
        char *end;
        errno = 0;
        value = strtoll(at, &end, 10);
        if (errno != 0) return -1;
        at = end;
    }
    const char *units = "KMGTPE";
    suffix[0] = 0;
    long long scale = 1;
    if (*at != 0) {
        /* Either case, as du takes it: -Bm is -BM, BLOCK_SIZE=kiB is KiB. */
        char letter = *at >= 'a' && *at <= 'z' ? (char)(*at - 'a' + 'A') : *at;
        const char *unit = strchr(units, letter);
        if (unit == NULL) return -1;
        int power = (int)(unit - units) + 1;
        long long base = 1024;
        if (strcmp(at + 1, "B") == 0) base = 1000;
        else if (at[1] != 0 && strcmp(at + 1, "iB") != 0) return -1;
        for (int i = 0; i < power; i++) scale *= base;
        /* Printed as du prints it: "kB" for a thousand, "K", "KiB" and "MB" as they are. */
        const char *tail = base == 1000 ? "B" : at[1] != 0 ? "iB" : "";
        char shown = base == 1000 && letter == 'K' ? 'k' : letter;
        snprintf(suffix, 8, "%c%s", shown, tail);
    } else if (!*digits) {
        return -1;
    }
    if (value > (long long)(9.2e18 / (double)scale)) return -1;
    *out = (negative ? -value : value) * scale;
    return 0;
}

/* -B's value, or an environment variable's: sets the unit and its suffix. */
static int setBlockSize(Options *options, const char *text) {
    long long size;
    int digits;
    char suffix[8];
    if (parseSize(text, &size, &digits, suffix) != 0 || size <= 0) return -1;
    options->blockSize = size;
    options->grouping = text[0] == '\'';
    options->human = 0;
    if (digits) options->blockSuffix[0] = 0;
    else snprintf(options->blockSuffix, sizeof options->blockSuffix, "%s", suffix);
    return 0;
}

static void addExclude(Options *options, const char *pattern) {
    if (options->excludeCount == options->excludeCap) {
        options->excludeCap = options->excludeCap ? options->excludeCap * 2 : 8;
        options->excludes =
            (char **)realloc(options->excludes, (size_t)options->excludeCap * sizeof(char *));
    }
    size_t length = strlen(pattern);
    char *copy = (char *)malloc(length + 1);
    memcpy(copy, pattern, length + 1);
    options->excludes[options->excludeCount++] = copy;
}

/*
 * Every line of the file (`sep` '\n'), or every NUL-separated name (`sep` 0).
 * An empty name, or "-" read from standard input, is reported with where it
 * was ("-:1: invalid zero-length file name") and left out, and the others are
 * still counted, as du does.
 */
static char **readNames(const char *path, char sep, int *count) {
    FILE *in = strcmp(path, "-") == 0 ? stdin : fopen(path, "rb");
    if (in == NULL) {
        fprintf(stderr, "%s: cannot open '%s' for reading: %s\n", programName, path,
                strerror(errno));
        exit(1);
    }
    size_t cap = 1 << 16, used = 0;
    char *text = (char *)malloc(cap);
    size_t got;
    while ((got = fread(text + used, 1, cap - used, in)) > 0) {
        used += got;
        if (used == cap) text = (char *)realloc(text, cap *= 2);
    }
    /* A folder opens, and fails only when read: "du: dir: read error: Is a directory". */
    if (ferror(in)) {
        fprintf(stderr, "%s: %s: read error: %s\n", programName, path, strerror(errno));
        exit(1);
    }
    if (in != stdin) fclose(in);
    char **names = (char **)malloc((used + 2) * sizeof(char *));
    *count = 0;
    size_t start = 0;
    long item = 0;
    int fromStdin = strcmp(path, "-") == 0;
    for (size_t i = 0; i <= used; i++) {
        if (i < used && text[i] != sep) continue;
        size_t length = i - start;
        if (sep == '\n' && length > 0 && text[i - 1] == '\r') length--;
        if (i < used || length > 0) item++;
        if (sep == 0 && length == 1 && text[start] == '-' && fromStdin) {
            fprintf(stderr,
                    "%s: when reading file names from standard input, no file name of '-' "
                    "allowed\n",
                    programName);
            badNames = 1;
        } else if (length > 0) {
            names[*count] = (char *)malloc(length + 1);
            memcpy(names[*count], text + start, length);
            names[*count][length] = 0;
            (*count)++;
        } else if (sep == 0 && i < used) {
            fprintf(stderr, "%s: %s:%ld: invalid zero-length file name\n", programName, path,
                    item);
            badNames = 1;
        }
        start = i + 1;
    }
    free(text);
    return names;
}

static const char *TIME_STYLES[][2] = {
    {"full-iso", "%Y-%m-%d %H:%M:%S.%N %z"},
    {"long-iso", "%Y-%m-%d %H:%M"},
    {"iso", "%Y-%m-%d"},
};

static int setTimeStyle(Options *options, const char *style) {
    if (strncmp(style, "posix-", 6) == 0) style += 6;
    if (style[0] == '+') {
        /* ls's second line is the format for recent times; du takes the first. */
        const char *newline = strchr(style + 1, '\n');
        size_t length = newline ? (size_t)(newline - style - 1) : strlen(style + 1);
        if (length >= sizeof options->timeFormat) length = sizeof options->timeFormat - 1;
        memcpy(options->timeFormat, style + 1, length);
        options->timeFormat[length] = 0;
        return 0;
    }
    if (strcmp(style, "locale") == 0) style = "long-iso";
    for (size_t i = 0; i < sizeof TIME_STYLES / sizeof TIME_STYLES[0]; i++) {
        if (strcmp(style, TIME_STYLES[i][0]) == 0) {
            snprintf(options->timeFormat, sizeof options->timeFormat, "%s", TIME_STYLES[i][1]);
            return 0;
        }
    }
    return -1;
}

enum {
    LONG_APPARENT = 256,
    LONG_FILES0,
    LONG_INODES,
    LONG_SI,
    LONG_TIME,
    LONG_TIME_STYLE,
    LONG_EXCLUDE,
    LONG_FILES,
    LONG_HELP,
    LONG_VERSION,
};

typedef struct {
    const char *name;
    int argument; /* 0 none, 1 required, 2 optional (only as --name=value) */
    int code;
    int extra; /* fastdu's own: matched only in full, so it never makes du's ambiguous */
} LongOption;

static const LongOption LONG_OPTIONS[] = {
    {"null", 0, '0', 0},
    {"all", 0, 'a', 0},
    {"apparent-size", 0, LONG_APPARENT, 0},
    {"block-size", 1, 'B', 0},
    {"bytes", 0, 'b', 0},
    {"total", 0, 'c', 0},
    {"dereference-args", 0, 'D', 0},
    {"max-depth", 1, 'd', 0},
    {"files0-from", 1, LONG_FILES0, 0},
    {"human-readable", 0, 'h', 0},
    {"inodes", 0, LONG_INODES, 0},
    {"dereference", 0, 'L', 0},
    {"count-links", 0, 'l', 0},
    {"no-dereference", 0, 'P', 0},
    {"separate-dirs", 0, 'S', 0},
    {"si", 0, LONG_SI, 0},
    {"summarize", 0, 's', 0},
    {"threshold", 1, 't', 0},
    {"time", 2, LONG_TIME, 0},
    {"time-style", 1, LONG_TIME_STYLE, 0},
    {"exclude-from", 1, 'X', 0},
    {"exclude", 1, LONG_EXCLUDE, 0},
    {"one-file-system", 0, 'x', 0},
    {"help", 0, LONG_HELP, 0},
    {"version", 0, LONG_VERSION, 0},
    {"threads", 1, 'j', 1},
    {"progress", 0, 'p', 1},
    {"file-count", 0, LONG_FILES, 1},
};

/*
 * A long option by its name, or by a prefix only one of du's options has, as
 * getopt_long does. fastdu's own options count only written out in full, so
 * "--th" is still du's --threshold and "--files" its --files0-from.
 */
static const LongOption *longOption(const char *name, size_t length, const char *arg) {
    const LongOption *found = NULL;
    int ambiguous = 0;
    for (size_t i = 0; i < sizeof LONG_OPTIONS / sizeof LONG_OPTIONS[0]; i++) {
        const LongOption *option = &LONG_OPTIONS[i];
        if (strncmp(option->name, name, length) != 0) continue;
        if (strlen(option->name) == length) return option;
        if (option->extra) continue;
        if (found != NULL) ambiguous = 1;
        found = option;
    }
    if (ambiguous) failWith("option '%s' is ambiguous", arg);
    if (found == NULL) failWith("unrecognized option '%s'", arg);
    return found;
}

static const char *TIME_WORDS[][2] = {
    {"atime", "a"}, {"access", "a"}, {"use", "a"}, {"ctime", "c"}, {"status", "c"},
};

static void unitsOf(Options *options, long long size) {
    options->blockSize = size;
    options->blockSuffix[0] = 0;
    options->human = 0;
}

/* A size that is none, named as the option was spelled: "invalid -t argument 'SIZE'". */
static void badSize(const char *spelled, const char *value) {
    fprintf(stderr, "%s: invalid %s argument '%s'\n", programName, spelled, value);
    exit(1);
}

/* One option; `spelled` is how messages name it: "-t", or "--threshold" however shortened. */
static void apply(Options *options, int code, const char *value, int *maxDepthGiven,
                  const char *spelled) {
    int digits;
    char suffix[8];
    switch (code) {
    case '0': options->nul = 1; break;
    case 'a': options->all = 1; break;
    case 'A':
    case LONG_APPARENT: options->apparent = 1; break;
    case 'B':
        if (setBlockSize(options, value) != 0) badSize(spelled, value);
        break;
    case 'b':
        options->apparent = 1;
        unitsOf(options, 1);
        break;
    case 'c': options->total = 1; break;
    case 'D':
    case 'H': options->derefArgs = 1; break;
    case 'd': {
        char *end;
        long depth = strtol(value, &end, 10);
        if (end == value || *end != 0 || depth < 0 || depth > 1 << 30)
            failWith("invalid maximum depth '%s'", value);
        options->depth = (int)depth;
        *maxDepthGiven = 1;
        break;
    }
    case 'h': options->human = 1024; break;
    case LONG_SI: options->human = 1000; break;
    case LONG_INODES: options->inodes = 1; break;
    case 'k': unitsOf(options, 1024); break;
    case 'm': unitsOf(options, 1024 * 1024); break;
    case 'L': options->deref = 1; break;
    case 'P':
        options->deref = 0;
        options->derefArgs = 0;
        break;
    case 'l': options->countLinks = 1; break;
    case 'S': options->separateDirs = 1; break;
    case 's': options->summarize = 1; break;
    case 't':
        if (parseSize(value, &options->threshold, &digits, suffix) != 0 || !digits)
            badSize(spelled, value);
        /* A negative zero is refused in du's own words, whichever way -t was spelled. */
        if (options->threshold == 0 && value[0] == '-') badSize("--threshold", value);
        break;
    case LONG_TIME: {
        options->time = TIME_MODIFIED;
        if (value == NULL) break;
        size_t words = sizeof TIME_WORDS / sizeof TIME_WORDS[0];
        size_t i = 0;
        while (i < words && strcmp(value, TIME_WORDS[i][0]) != 0) i++;
        if (i == words) failWith("invalid argument '%s' for '--time'", value);
        options->time = TIME_WORDS[i][1][0] == 'a' ? TIME_ACCESSED : TIME_CHANGED;
        break;
    }
    case LONG_TIME_STYLE:
        if (setTimeStyle(options, value) != 0)
            failWith("invalid argument '%s' for 'time style'", value);
        break;
    case 'X': {
        int count;
        char **patterns = readNames(value, '\n', &count);
        for (int i = 0; i < count; i++) addExclude(options, patterns[i]);
        break;
    }
    case LONG_EXCLUDE: addExclude(options, value); break;
    case 'x': options->oneFileSystem = 1; break;
    case 'j': {
        char *end;
        long threads = strtol(value, &end, 10);
        if (end == value || *end != 0 || threads < 1 || threads > 4096)
            failWith("invalid thread count '%s'", value);
        options->threads = (int)threads;
        break;
    }
    case 'p': options->progress = 1; break;
    case LONG_FILES: options->files = 1; break;
    case LONG_HELP:
        printf("Usage: %s [OPTION]... [FILE]...\n  or:  %s [OPTION]... --files0-from=F\n",
               programName, programName);
        fputs(USAGE, stdout);
        exit(0);
    case LONG_VERSION:
        printf("fastdu %s\n", VERSION);
        exit(0);
    default: failWith("invalid option -- '%s'", spelled + 1);
    }
}

/* Exits after --help and --version, and on a mistake, with du's words. */
static void parseOptions(int argc, char **argv, Options *options) {
    memset(options, 0, sizeof *options);
    if (argc > 0) setProgramName(argv[0]);
    /* The locale's digit grouping (-B "'1") and month names (--time-style=+%b), as du's. */
    setlocale(LC_ALL, "");
    options->depth = -1;
    options->blockSize = getenv("POSIXLY_CORRECT") != NULL ? 512 : 1024;
    const char *variables[] = {"DU_BLOCK_SIZE", "BLOCK_SIZE", "BLOCKSIZE"};
    for (int i = 0; i < 3; i++) {
        const char *value = getenv(variables[i]);
        if (value != NULL && setBlockSize(options, value) == 0) break;
    }
    const char *style = getenv("TIME_STYLE");
    if (style == NULL || setTimeStyle(options, style) != 0) setTimeStyle(options, "long-iso");
    options->roots = (char **)malloc((size_t)(argc + 1) * sizeof(char *));
    const char *files0 = NULL;
    int maxDepthGiven = 0, onlyPaths = 0;
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
        if (arg[1] == '-') {
            const char *equals = strchr(arg + 2, '=');
            size_t length = equals ? (size_t)(equals - arg - 2) : strlen(arg + 2);
            const LongOption *option = longOption(arg + 2, length, arg);
            const char *value = equals ? equals + 1 : NULL;
            if (option->argument == 0 && value != NULL)
                failWith("option '--%s' doesn't allow an argument", option->name);
            if (option->argument == 1 && value == NULL) {
                if (i + 1 == argc) failWith("option '--%s' requires an argument", option->name);
                value = argv[++i];
            }
            char spelled[64];
            snprintf(spelled, sizeof spelled, "--%s", option->name);
            if (option->code == LONG_FILES0) files0 = value;
            else apply(options, option->code, value, &maxDepthGiven, spelled);
            continue;
        }
        /* Short flags bundle (-sh); a value follows its flag (-d1) or comes next. */
        for (const char *flag = arg + 1; *flag != 0; flag++) {
            char letter[2] = {*flag, 0};
            char spelled[3] = {'-', *flag, 0};
            if (strchr("0aAbcDHhkLlmPSsxp", *flag) != NULL) {
                apply(options, *flag, NULL, &maxDepthGiven, spelled);
                continue;
            }
            if (strchr("BdtXj", *flag) == NULL) failWith("invalid option -- '%s'", letter);
            const char *value = flag[1] != 0 ? flag + 1 : i + 1 < argc ? argv[++i] : NULL;
            if (value == NULL) failWith("option requires an argument -- '%s'", letter);
            apply(options, *flag, value, &maxDepthGiven, spelled);
            break;
        }
    }
    if (options->summarize && options->all)
        failWith("%s", "cannot both summarize and show all entries");
    if (options->summarize && maxDepthGiven && options->depth != 0) {
        fprintf(stderr, "%s: warning: summarizing conflicts with --max-depth=%d\n", programName,
                options->depth);
        tryHelp();
    }
    if (options->summarize) options->depth = 0;
    if (options->inodes) {
        if (options->apparent)
            fprintf(stderr,
                    "%s: warning: options --apparent-size and -b are ineffective with --inodes\n",
                    programName);
        options->blockSize = 1;
        options->blockSuffix[0] = 0;
    }
    if (files0 != NULL) {
        if (options->rootCount > 0) {
            fprintf(stderr, "%s: extra operand '%s'\n", programName, options->roots[0]);
            fputs("file operands cannot be combined with --files0-from\n", stderr);
            tryHelp();
        }
        options->roots = readNames(files0, 0, &options->rootCount);
        return;
    }
    if (options->rootCount == 0) options->roots[options->rootCount++] = (char *)".";
}

#endif
