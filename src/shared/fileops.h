/* fileops: native file management for vabridge (bridge 5.0, 2026-09-24).
 *
 * Runs inside SceShell with the rest of the bridge, so it works while a game
 * is running and needs neither Agent Recovery nor FTP. Built -nostdlib: only
 * sceIo / sceClib / sceKernel calls, no malloc.
 *
 * Streaming (put/get) happens on the client's connection. Tree operations
 * (copy, move, remove, sha256) run on one background job thread so a long copy
 * never blocks status probes or screenshots; poll them with fo_job_status.
 */
#ifndef VABRIDGE_FILEOPS_H
#define VABRIDGE_FILEOPS_H

#define FO_PATH_MAX 512

enum { FO_JOB_COPY = 1, FO_JOB_MOVE, FO_JOB_REMOVE, FO_JOB_SHA256, FO_JOB_FETCH };

/* Policy. 0 = allowed, negative = refused. Reads may touch any mounted
 * device; writes only the user partitions, never tai/ (a bad config.txt
 * bricks the boot chain), and removals never a device root or a top-level
 * folder like ux0:app. */
int fo_can_read(const char *path);
int fo_can_write(const char *path);
int fo_can_remove(const char *path);

/* Synchronous, cheap. */
int fo_mkdirs(const char *path);                         /* mkdir -p */
int fo_stat_line(const char *path, char *out, int max);  /* "OK stat d|f <size> <mtime>" */

/* Streaming on socket s. fo_put reads exactly `size` raw bytes after the
 * command line, writes them to path.part, then renames into place, so a
 * dropped transfer never leaves a truncated file under the real name. */
int fo_put(int s, const char *path, unsigned long long size, char *out, int max);
int fo_get(int s, const char *path);

/* Background jobs, one at a time. */
int fo_job_start(int kind, const char *src, const char *dst);  /* job id, or <0 */
/* HTTP(S) download (fetch.c). expect_sha: 64 hex chars or NULL; see fetch.h
 * for why it is also the only way past a certificate the Vita cannot check. */
int fo_job_start_fetch(const char *url, const char *dst, const char *expect_sha);
void fo_job_status(char *out, int max);
int fo_job_cancel(void);

/* Structured status for a UI (the text line above is for the wire). */
enum { FO_ST_NONE, FO_ST_RUNNING, FO_ST_DONE, FO_ST_FAILED, FO_ST_CANCELLED };
typedef struct {
    int id, kind, state, phase, rc;
    unsigned int files_done, files_total;
    unsigned long long bytes_done, bytes_total;
    char note[80];
} FoJobInfo;
void fo_job_info(FoJobInfo *out);

/* Rename within one partition, under the same rules as a move. */
int fo_rename(const char *src, const char *dst);
int fo_shutdown(void);    /* module_stop: cancel and wait; <0 if the job will not stop */

#endif
