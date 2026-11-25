// Files written in the background. On the 3DS one low-priority thread on the app's core does the SD card's slow calls
// (a save's replace took 366 ms) while the game runs; elsewhere a write happens at once.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Takes the malloc'd [buf] and frees it. [path] gets it as a whole file or keeps its old one: written to path.tmp, the
// old file set aside as path.bak, the new one renamed in, the .bak removed. A job for [path] still waiting is dropped
// for this one. *failed (may be NULL) is set to 1 if the write fails.
void pd_writer_write(const char* path, uint8_t* buf, size_t len, volatile int* failed);
// As pd_writer_write, and: [aside] (may be NULL) is where the file it replaces is renamed to first, replacing an older
// one there; [first] puts it ahead of the other waiting jobs (a game's own save).
void pd_writer_write_ex(const char* path, uint8_t* buf, size_t len, volatile int* failed, const char* aside, bool first);
// The next write to [path] sets its old file aside as [aside] (NULL: cancels).
void pd_writer_aside_next(const char* path, const char* aside);
// File [from] moved to [to] (replacing it) when the job runs, if it's there then.
void pd_writer_rename(const char* from, const char* to);
// fn(a, b) on the writer's thread, after the jobs queued before it. It must not touch the app's state.
void pd_writer_call(void (*fn)(const char* a, const char* b), const char* a, const char* b);
// A malloc'd copy of the newest bytes queued or being written for [path], or NULL if none: a reader needs no drain.
uint8_t* pd_writer_pending_copy(const char* path, size_t* len);
// True while a write to or a move into [path] is queued or running.
bool pd_writer_has(const char* path);
// Waits for every queued job. Before a game closes, restarts or loads another, and before the app sleeps or exits.
void pd_writer_drain(void);
// What the background thread did, to the log from the caller's thread (never from the writer's own).
void pd_writer_set_log(void (*log)(const char* line));
void pd_writer_poll(void);
// Stops the thread after the queue is done.
void pd_writer_exit(void);
