#include "pd_writer.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef __3DS__
#include <3ds.h>
#endif

#define PATH_LEN 512
#define QUEUE 6
#define QUEUED_BYTES_MAX (3u << 20)
#define REPORTS 8
#define REPORT_LEN 200
#define WRITER_STACK 16384

enum kind { WRITE, RENAME, CALL };

struct job {
    enum kind kind;
    char path[PATH_LEN], from[PATH_LEN];
    char aside[PATH_LEN]; // a WRITE: the file it replaces is renamed to this first (empty: the .bak way)
    uint8_t* buf;
    size_t len;
    volatile int* failed;
    void (*fn)(const char* a, const char* b);
};

static struct job queue[QUEUE];
static unsigned queued;
static bool busy;
static char runningPath[PATH_LEN];
static uint8_t* runningBuf; // the running WRITE's bytes, until the writer frees them
static size_t runningLen;
static char reports[REPORTS][REPORT_LEN];
static unsigned reportsHead, reportsCount;
static void (*logFn)(const char*);

#ifdef __3DS__
static LightLock lock;
static LightEvent wake;
static Thread thread;
static bool threadTried, threadStop;
static size_t queuedBytes;
static bool lockReady;
static aptHookCookie sleepHook;
// A zeroed LightLock isn't an unlocked one: set up by whoever comes first (the game's own thread).
#define LOCK() (lockReady ? (void) 0 : (LightLock_Init(&lock), lockReady = true), LightLock_Lock(&lock))
#define UNLOCK() LightLock_Unlock(&lock)
#else
#define LOCK() ((void) 0)
#define UNLOCK() ((void) 0)
#endif

static void report(const char* format, ...) __attribute__((format(printf, 1, 2)));
static void report(const char* format, ...) {
    LOCK();
    char* line = reports[(reportsHead + reportsCount) % REPORTS];
    if (reportsCount == REPORTS) reportsHead = (reportsHead + 1) % REPORTS;
    else reportsCount++;
    va_list args;
    va_start(args, format);
    vsnprintf(line, REPORT_LEN, format, args);
    va_end(args);
    UNLOCK();
}

// path.tmp is whole: it takes path's place, and path's old file stays as path.bak until it has.
static bool commit(const char* temp, const char* path) {
    char bak[PATH_LEN + 8];
    snprintf(bak, sizeof bak, "%s.bak", path);
    struct stat st;
    bool had = stat(path, &st) == 0;
    if (had) {
        remove(bak);
        if (rename(path, bak) != 0) return false;
    }
    if (rename(temp, path) != 0) {
        if (had) rename(bak, path);
        return false;
    }
    if (had) remove(bak);
    return true;
}

static bool run_job(const struct job* j) {
    if (j->kind == CALL) {
        j->fn(j->path, j->from);
        return true;
    }
    if (j->kind == RENAME) {
        struct stat st;
        if (stat(j->from, &st) != 0) return true; // nothing to move
        remove(j->path);
        return rename(j->from, j->path) == 0;
    }
    char temp[PATH_LEN + 8];
    snprintf(temp, sizeof temp, "%s.tmp", j->path);
    FILE* out = fopen(temp, "wb");
    bool ok = out != NULL;
    if (ok) ok = fwrite(j->buf, 1, j->len, out) == j->len;
    if (out) ok = fclose(out) == 0 && ok;
    if (!ok) {
        remove(temp);
        return false;
    }
    if (j->kind == WRITE && j->aside[0]) {
        struct stat st;
        if (stat(j->path, &st) == 0) {
            remove(j->aside);
            rename(j->path, j->aside);
        }
    }
    return commit(temp, j->path);
}

static void finish(struct job* j) {
    bool ok = run_job(j);
    if (!ok) {
        report("writer: COULDN'T WRITE %s", j->path);
        if (j->failed) *j->failed = 1;
    }
}

#ifdef __3DS__
static void writer_main(void* arg) {
    (void) arg;
    for (;;) {
        LOCK();
        if (!queued) {
            busy = false;
            runningPath[0] = 0;
            bool stop = threadStop;
            UNLOCK();
            if (stop) break;
            LightEvent_Wait(&wake);
            continue;
        }
        struct job j = queue[0];
        memmove(queue, queue + 1, (queued - 1) * sizeof(struct job));
        queued--;
        queuedBytes -= j.len;
        busy = true;
        snprintf(runningPath, sizeof runningPath, "%s", j.path);
        runningBuf = j.kind == WRITE ? j.buf : NULL;
        runningLen = j.len;
        UNLOCK();
        finish(&j);
        LOCK();
        runningBuf = NULL;
        UNLOCK();
        free(j.buf);
    }
}

static void sleep_hook(APT_HookType hook, void* param) {
    (void) param;
    if (hook == APTHOOK_ONSUSPEND || hook == APTHOOK_ONSLEEP || hook == APTHOOK_ONEXIT) pd_writer_drain();
}

// On the app's own core, under the game thread: it runs while that waits for a frame, and an Old 3DS refuses a second
// app thread on the other core.
static void start_thread(void) {
    if (threadTried) return;
    threadTried = true;
    LOCK();
    UNLOCK();
    LightEvent_Init(&wake, RESET_ONESHOT);
    aptHook(&sleepHook, sleep_hook, NULL);
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    s32 low = priority + 2 > 0x3F ? 0x3F : priority + 2;
    thread = threadCreate(writer_main, NULL, WRITER_STACK, low, 0, false);
}
#endif

static bool threaded(void) {
#ifdef __3DS__
    start_thread();
    return thread != NULL;
#else
    return false;
#endif
}

#ifdef __3DS__
static void wait_room(size_t len) {
    for (;;) {
        LOCK();
        bool room = queued < QUEUE && (!queued || queuedBytes + len <= QUEUED_BYTES_MAX);
        UNLOCK();
        if (room) return;
        svcSleepThread(2 * 1000 * 1000);
    }
}
#endif

static void enqueue(struct job* j, bool first) {
    if (!threaded()) {
        finish(j);
        free(j->buf);
        return;
    }
    (void) first;
#ifdef __3DS__
    wait_room(j->len);
    LOCK();
    // A write still waiting for the same file is out of date, unless a job queued after it reads that file.
    for (int i = (int) queued - 1; i >= 0 && j->kind == WRITE; i--) {
        const struct job* q = &queue[i];
        if (q->kind == CALL || (q->kind == RENAME && !strcmp(q->from, j->path))) break;
        if (q->kind == WRITE && !strcmp(q->path, j->path)) {
            if (!j->aside[0]) memcpy(j->aside, q->aside, sizeof j->aside);
            free(q->buf);
            queuedBytes -= q->len;
            memmove(queue + i, queue + i + 1, (queued - i - 1) * sizeof(struct job));
            queued--;
            break;
        }
    }
    if (first) memmove(queue + 1, queue, queued * sizeof(struct job));
    queue[first ? 0 : queued] = *j;
    queued++;
    queuedBytes += j->len;
    UNLOCK();
    LightEvent_Signal(&wake);
#endif
}

static char nextAsidePath[PATH_LEN], nextAside[PATH_LEN];

void pd_writer_aside_next(const char* path, const char* aside) {
    snprintf(nextAsidePath, sizeof nextAsidePath, "%s", path ? path : "");
    snprintf(nextAside, sizeof nextAside, "%s", aside ? aside : "");
}

void pd_writer_write_ex(const char* path, uint8_t* buf, size_t len, volatile int* failed, const char* aside, bool first) {
    struct job j = { .kind = WRITE, .buf = buf, .len = len, .failed = failed };
    if (!aside && nextAsidePath[0] && !strcmp(nextAsidePath, path)) {
        aside = nextAside;
        nextAsidePath[0] = 0;
    }
    if (snprintf(j.path, sizeof j.path, "%s", path) >= (int) sizeof j.path ||
        (aside && snprintf(j.aside, sizeof j.aside, "%s", aside) >= (int) sizeof j.aside)) {
        free(buf);
        if (failed) *failed = 1;
        return;
    }
    enqueue(&j, first);
}

void pd_writer_write(const char* path, uint8_t* buf, size_t len, volatile int* failed) {
    pd_writer_write_ex(path, buf, len, failed, NULL, false);
}

void pd_writer_rename(const char* from, const char* to) {
    struct job j = { .kind = RENAME };
    if (snprintf(j.path, sizeof j.path, "%s", to) >= (int) sizeof j.path || snprintf(j.from, sizeof j.from, "%s", from) >= (int) sizeof j.from) return;
    enqueue(&j, false);
}

void pd_writer_call(void (*fn)(const char* a, const char* b), const char* a, const char* b) {
    struct job j = { .kind = CALL, .fn = fn };
    if (snprintf(j.path, sizeof j.path, "%s", a) >= (int) sizeof j.path || snprintf(j.from, sizeof j.from, "%s", b) >= (int) sizeof j.from) return;
    enqueue(&j, false);
}

uint8_t* pd_writer_pending_copy(const char* path, size_t* len) {
    uint8_t* out = NULL;
    LOCK();
    const uint8_t* src = NULL;
    size_t n = 0;
    bool newer = false; // a move into the file comes after: its bytes aren't in any buffer
    for (int i = (int) queued - 1; i >= 0 && !src && !newer; i--) {
        if (strcmp(queue[i].path, path)) continue;
        if (queue[i].kind == WRITE) {
            src = queue[i].buf;
            n = queue[i].len;
        } else {
            newer = true;
        }
    }
    if (!src && !newer && busy && runningBuf && !strcmp(runningPath, path)) {
        src = runningBuf;
        n = runningLen;
    }
    if (src) {
        out = malloc(n ? n : 1);
        if (out) memcpy(out, src, n);
        if (out) *len = n;
    }
    UNLOCK();
    return out;
}

bool pd_writer_has(const char* path) {
    bool has = false;
    LOCK();
    for (unsigned i = 0; i < queued; i++)
        if (!strcmp(queue[i].path, path)) has = true;
    if (busy && !strcmp(runningPath, path)) has = true;
    UNLOCK();
    return has;
}

void pd_writer_drain(void) {
#ifdef __3DS__
    if (thread) {
        for (;;) {
            LOCK();
            bool idle = !queued && !busy;
            UNLOCK();
            if (idle) break;
            svcSleepThread(2 * 1000 * 1000);
        }
    }
#endif
    pd_writer_poll();
}

void pd_writer_set_log(void (*log)(const char* line)) {
    logFn = log;
}

void pd_writer_poll(void) {
    for (;;) {
        char line[REPORT_LEN];
        LOCK();
        bool any = reportsCount > 0;
        if (any) {
            memcpy(line, reports[reportsHead], sizeof line);
            reportsHead = (reportsHead + 1) % REPORTS;
            reportsCount--;
        }
        UNLOCK();
        if (!any) return;
        if (logFn) logFn(line);
    }
}

void pd_writer_exit(void) {
#ifdef __3DS__
    if (!thread) return;
    pd_writer_drain();
    LOCK();
    threadStop = true;
    UNLOCK();
    LightEvent_Signal(&wake);
    threadJoin(thread, U64_MAX);
    threadFree(thread);
    aptUnhook(&sleepHook);
    thread = NULL;
    threadTried = false;
    threadStop = false;
#endif
}
