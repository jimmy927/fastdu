/*
 * How fastdu prints, the way GNU du does: sizes rounded up to the unit, -h and
 * --si as GNU's human_readable rounds them, times in a --time-style, and which
 * names --exclude leaves out. Shared by both walkers.
 */

#ifndef FASTDU_OUTPUT_H
#define FASTDU_OUTPUT_H

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "options.h"

/*
 * -h and --si: GNU's human_readable with autoscale and rounding up. Under the
 * base the number is printed whole; above, it is scaled until it is under the
 * base, printed with one decimal while under 10, and always rounded up.
 */
static int humanSize(char *out, unsigned long long amount, unsigned base) {
    if (amount < base) return sprintf(out, "%llu", amount);
    const char *letters = base == 1000 ? "kMGTPEZY" : "KMGTPEZY";
    unsigned tenths = 0, rounding = 0;
    int exponent = 0;
    do {
        unsigned r10 = (unsigned)(amount % base) * 10 + tenths;
        unsigned r2 = (r10 % base) * 2 + (rounding >> 1);
        amount /= base;
        tenths = r10 / base;
        rounding = r2 < base ? (r2 != 0) : 2 + (base < r2);
        exponent++;
    } while (base <= amount && exponent < 8);
    if (amount < 10) {
        if (tenths + rounding > 0) {
            tenths++;
            rounding = 0;
            if (tenths == 10) {
                amount++;
                tenths = 0;
            }
        }
        if (amount < 10) return sprintf(out, "%llu.%u%c", amount, tenths, letters[exponent - 1]);
        tenths = rounding = 0;
    }
    if (tenths + rounding > 0) {
        amount++;
        if (amount == base && exponent < 8) return sprintf(out, "1.0%c", letters[exponent]);
    }
    return sprintf(out, "%llu%c", amount, letters[exponent - 1]);
}

/* A size in bytes, or a count of inodes, as the options print it. */
static int formatQuantity(char *out, const Options *options, long long amount) {
    unsigned long long value = amount < 0 ? 0 : (unsigned long long)amount;
    if (options->human != 0) return humanSize(out, value, (unsigned)options->human);
    unsigned long long unit = (unsigned long long)options->blockSize;
    unsigned long long blocks = value / unit + (value % unit != 0);
    return sprintf(out, "%llu%s", blocks, options->blockSuffix);
}

/* A time, in nanoseconds since 1970, in the --time-style (local time, %N added). */
static void formatTime(char *out, size_t size, const Options *options, long long nanoseconds) {
    time_t seconds = (time_t)(nanoseconds / 1000000000);
    long nanos = (long)(nanoseconds % 1000000000);
    if (nanos < 0) {
        seconds -= 1;
        nanos += 1000000000;
    }
    char format[512];
    size_t used = 0;
    for (const char *at = options->timeFormat; *at != 0 && used + 10 < sizeof format; at++) {
        if (at[0] == '%' && at[1] == 'N') {
            used += (size_t)sprintf(format + used, "%09ld", nanos);
            at++;
        } else if (at[0] == '%' && at[1] == '%') {
            format[used++] = '%';
            format[used++] = '%';
            at++;
        } else {
            format[used++] = *at;
        }
    }
    format[used] = 0;
    struct tm local;
#ifdef _WIN32
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    if (strftime(out, size, format, &local) == 0) out[0] = 0;
}

/* fnmatch without flags, as du matches: * ? [set] [!set] and \ escapes. */
static int globMatch(const char *pattern, const char *name) {
    for (;;) {
        char p = *pattern;
        if (p == 0) return *name == 0;
        if (p == '*') {
            while (*pattern == '*') pattern++;
            if (*pattern == 0) return 1;
            for (const char *rest = name; *rest != 0; rest++) {
                if (globMatch(pattern, rest)) return 1;
            }
            return globMatch(pattern, "");
        }
        if (*name == 0) return 0;
        if (p == '?') {
            pattern++;
            name++;
            continue;
        }
        if (p == '[') {
            const char *at = pattern + 1;
            int negate = *at == '!' || *at == '^';
            if (negate) at++;
            int matched = 0, first = 1;
            while (*at != 0 && (first || *at != ']')) {
                first = 0;
                char low = *at, high = *at;
                if (at[1] == '-' && at[2] != 0 && at[2] != ']') {
                    high = at[2];
                    at += 2;
                }
                if (*name >= low && *name <= high) matched = 1;
                at++;
            }
            if (*at != ']') {
                /* No closing bracket: the [ is itself. */
                if (*name != '[') return 0;
                pattern++;
                name++;
                continue;
            }
            if (matched == negate) return 0;
            pattern = at + 1;
            name++;
            continue;
        }
        if (p == '\\' && pattern[1] != 0) p = *++pattern;
        if (p != *name) return 0;
        pattern++;
        name++;
    }
}

static int isSeparator(char c) {
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

/*
 * Whether --exclude or -X leaves this entry out. As in du, a pattern is matched
 * against the whole path and against every part of it that follows a slash,
 * so "node_modules" and "*.log" match by name and "a/b" by the end of the path.
 */
static int excluded(const Options *options, const char *path) {
    for (int i = 0; i < options->excludeCount; i++) {
        const char *pattern = options->excludes[i];
        if (globMatch(pattern, path)) return 1;
        for (const char *at = path; *at != 0; at++) {
            if (isSeparator(*at) && at[1] != 0 && globMatch(pattern, at + 1)) return 1;
        }
    }
    return 0;
}

/* Whether any pattern needs the whole path, not only the entry's name. */
static int excludesNeedPaths(const Options *options) {
    for (int i = 0; i < options->excludeCount; i++) {
        for (const char *at = options->excludes[i]; *at != 0; at++) {
            if (isSeparator(*at)) return 1;
        }
    }
    return 0;
}

/* Whether -t lets this entry be printed. */
static int shownBy(const Options *options, long long amount) {
    long long threshold = options->threshold;
    if (threshold > 0) return amount >= threshold;
    if (threshold < 0) return amount <= -threshold;
    return 1;
}

/* The start of a line: the size, the --files column and the --time column. */
static void printLead(FILE *out, const Options *options, long long amount, long long files,
                      long long nanoseconds) {
    char text[64];
    formatQuantity(text, options, amount);
    fputs(text, out);
    fputc('\t', out);
    if (options->files) fprintf(out, "%lld\t", files);
    if (options->time != TIME_NONE) {
        char when[256];
        formatTime(when, sizeof when, options, nanoseconds);
        fputs(when, out);
        fputc('\t', out);
    }
}

static void endLine(FILE *out, const Options *options) { fputc(options->nul ? 0 : '\n', out); }

#endif
