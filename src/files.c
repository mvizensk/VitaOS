#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <psp2/ctrl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <psp2/io/devctl.h>

#include <psp2/kernel/threadmgr.h>
#include "files.h"
#include "shared/fileops.h"

typedef struct {
    char name[256];
    unsigned long long size;
    int dir, mark;
} Entry;

typedef struct {
    char path[FO_PATH_MAX];      /* "" = the device list */
    Entry *e;
    int n, cap, sel;
    float top;                   /* first visible row, eased */
    volatile int loading;        /* a background read of path is under way */
    char keep[256];              /* the name to select once it lands */
    Entry *next_e;               /* what the reader found, swapped in on the main thread */
    int next_n, next_cap;
    volatile int ready;
} Pane;

static Pane pane[2];
static int active;

#define LIST_Y 112
#define ROW_H 34
#define ROWS 10                  /* leaves room for the job bar above the footer */
#define PANE_W 462

/* ---------- queued operations ---------- */

typedef struct { int kind; char src[FO_PATH_MAX], dst[FO_PATH_MAX]; } Op;
#define MAX_OPS 256
static Op ops[MAX_OPS];
static int op_head, op_tail, op_total, op_done, op_failed;
static char last_error[160];
static int reload_after;         /* job finished: refresh both panes */

static void queue_op(int kind, const char *src, const char *dst) {
    if (op_tail - op_head >= MAX_OPS) return;
    Op *o = &ops[op_tail % MAX_OPS];
    o->kind = kind;
    snprintf(o->src, sizeof(o->src), "%s", src);
    snprintf(o->dst, sizeof(o->dst), "%s", dst ? dst : "");
    ++op_tail;
    ++op_total;
}

int files_busy(void) {
    FoJobInfo j;
    fo_job_info(&j);
    return j.state == FO_ST_RUNNING || op_head != op_tail;
}

/* Called every frame: start the next queued item once the job thread is free. */
static void pump_ops(void) {
    FoJobInfo j;
    fo_job_info(&j);
    static int watching;          /* id of the job we started, 0 = none */
    if (watching && j.id == watching && j.state != FO_ST_RUNNING) {
        if (j.state == FO_ST_FAILED)
            snprintf(last_error, sizeof(last_error), "rc 0x%08X at %s", j.rc, j.note);
        if (j.state == FO_ST_FAILED) ++op_failed;
        else ++op_done;
        watching = 0;
        reload_after = 1;
    }
    if (watching || j.state == FO_ST_RUNNING || op_head == op_tail) {
        if (op_head == op_tail && !watching && op_total) {
            if (op_failed) {
                char msg[240];
                snprintf(msg, sizeof(msg), "%d of %d items failed.\nLast error: %s",
                         op_failed, op_total, last_error);
                ui_message("Some items failed", msg);
            }
            op_total = op_done = op_failed = 0;
        }
        return;
    }
    Op *o = &ops[op_head % MAX_OPS];
    ++op_head;
    int id = fo_job_start(o->kind, o->src, o->kind == FO_JOB_REMOVE || o->kind == FO_JOB_SHA256 ? NULL : o->dst);
    if (id < 0) {
        snprintf(last_error, sizeof(last_error), "rc 0x%08X refusing %s", id, o->src);
        ++op_failed;
    } else {
        watching = id;
    }
}

/* ---------- listing ---------- */

static const char *const DEVICES[] = {"ux0:", "ur0:", "uma0:", "imc0:", "xmc0:", "grw0:",
                                      "ud0:", "pd0:", "os0:", "vs0:", "sa0:", "tm0:"};

static void push_entry(Pane *p, const char *name, unsigned long long size, int dir) {
    if (p->n == p->cap) {
        int cap = p->cap ? p->cap * 2 : 128;
        Entry *e = realloc(p->e, cap * sizeof(Entry));
        if (!e) return;
        p->e = e;
        p->cap = cap;
    }
    Entry *e = &p->e[p->n++];
    snprintf(e->name, sizeof(e->name), "%s", name);
    e->size = size;
    e->dir = dir;
    e->mark = 0;
}

static int by_name(const void *a, const void *b) {
    const Entry *x = a, *y = b;
    if (x->dir != y->dir) return y->dir - x->dir;          /* folders first */
    return strcasecmp(x->name, y->name);
}

/* Reading a folder or the device list touches the card, which can block for
 * seconds here: a reader thread does it, and the pane shows "Loading" until
 * the result is swapped in (the Files tab once froze for 9.6 s). */
static SceUID reader_sema = -1;
static char dev_line_cache[12][48];

static void push_next(Pane *p, const char *name, unsigned long long size, int dir) {
    if (p->next_n == p->next_cap) {
        int cap = p->next_cap ? p->next_cap * 2 : 128;
        Entry *e = realloc(p->next_e, cap * sizeof(Entry));
        if (!e) return;
        p->next_e = e;
        p->next_cap = cap;
    }
    Entry *e = &p->next_e[p->next_n++];
    snprintf(e->name, sizeof(e->name), "%s", name);
    e->size = size;
    e->dir = dir;
    e->mark = 0;
}

static void read_pane(Pane *p) {
    p->next_n = 0;
    if (!p->path[0]) {
        for (unsigned int i = 0; i < sizeof(DEVICES) / sizeof(DEVICES[0]); ++i) {
            SceUID d = sceIoDopen(DEVICES[i]);
            if (d < 0) continue;
            sceIoDclose(d);
            push_next(p, DEVICES[i], 0, 1);
            SceIoDevInfo info;
            memset(&info, 0, sizeof(info));
            dev_line_cache[i][0] = 0;
            if (sceIoDevctl(DEVICES[i], 0x3001, NULL, 0, &info, sizeof(info)) >= 0 && info.max_size) {
                char fr[32], tot[32];
                human_size(info.free_size, fr, sizeof(fr));
                human_size(info.max_size, tot, sizeof(tot));
                snprintf(dev_line_cache[i], sizeof(dev_line_cache[i]), "%s free of %s", fr, tot);
            }
        }
    } else {
        SceUID d = sceIoDopen(p->path);
        if (d >= 0) {
            SceIoDirent e;
            for (;;) {
                memset(&e, 0, sizeof(e));
                if (sceIoDread(d, &e) <= 0) break;
                if (!strcmp(e.d_name, ".") || !strcmp(e.d_name, "..")) continue;
                push_next(p, e.d_name, e.d_stat.st_size, SCE_S_ISDIR(e.d_stat.st_mode));
            }
            sceIoDclose(d);
            qsort(p->next_e, p->next_n, sizeof(Entry), by_name);
        }
    }
}

static int reader(SceSize args, void *argp) {
    (void)args; (void)argp;
    for (;;) {
        sceKernelWaitSema(reader_sema, 1, NULL);
        for (int i = 0; i < 2; ++i)
            if (pane[i].loading && !pane[i].ready) { read_pane(&pane[i]); pane[i].ready = 1; }
    }
    return 0;
}

static void load(Pane *p, const char *keep) {
    if (reader_sema < 0) {
        reader_sema = sceKernelCreateSema("files_read", 0, 0, 2, NULL);
        SceUID t = sceKernelCreateThread("files_reader", reader, 0x10000100, 0x8000, 0, 0, NULL);
        if (t >= 0) sceKernelStartThread(t, 0, NULL);
    }
    snprintf(p->keep, sizeof(p->keep), "%s", keep ? keep : "");
    p->n = 0;                                          /* nothing to act on until it lands */
    p->ready = 0;
    p->loading = 1;
    sceKernelSignalSema(reader_sema, 1);
}

/* Main thread, every frame: take a finished read. */
static void take_loaded(Pane *p) {
    if (!p->loading || !p->ready) return;
    Entry *e = p->e; int cap = p->cap;
    p->e = p->next_e; p->n = p->next_n; p->cap = p->next_cap;
    p->next_e = e; p->next_cap = cap; p->next_n = 0;
    p->sel = 0;
    if (p->keep[0])
        for (int i = 0; i < p->n; ++i)
            if (!strcmp(p->e[i].name, p->keep)) { p->sel = i; break; }
    if (p->sel >= p->n) p->sel = p->n ? p->n - 1 : 0;
    p->loading = 0;
    p->ready = 0;
}

static void reload(Pane *p) {
    if (p->loading) return;
    char keep[256] = "";
    if (p->n) snprintf(keep, sizeof(keep), "%s", p->e[p->sel].name);
    load(p, keep);
}

static void join(char *out, int max, const char *dir, const char *name) {
    int n = (int)strlen(dir);
    if (!n) snprintf(out, max, "%s", name);
    else snprintf(out, max, "%s%s%s", dir, dir[n - 1] == ':' || dir[n - 1] == '/' ? "" : "/", name);
}

static void enter(Pane *p) {
    if (p->loading || !p->n || !p->e[p->sel].dir) return;
    char next[FO_PATH_MAX];
    join(next, sizeof(next), p->path, p->e[p->sel].name);
    snprintf(p->path, sizeof(p->path), "%s", next);
    load(p, NULL);
    p->top = 0;
}

static void go_up(Pane *p) {
    if (p->loading || !p->path[0]) return;
    char child[256];
    int n = (int)strlen(p->path);
    if (p->path[n - 1] == ':') {                        /* device root: back to the list */
        snprintf(child, sizeof(child), "%s", p->path);
        p->path[0] = 0;
    } else {
        char *slash = strrchr(p->path, '/');
        char *colon = strchr(p->path, ':');
        if (slash && slash > colon) {
            snprintf(child, sizeof(child), "%s", slash + 1);
            *slash = 0;
        } else {
            snprintf(child, sizeof(child), "%s", colon + 1);
            colon[1] = 0;
        }
    }
    load(p, child);
}

void files_init(void) {
    snprintf(pane[0].path, sizeof(pane[0].path), "ux0:");
    pane[1].path[0] = 0;
    load(&pane[0], NULL);
    load(&pane[1], NULL);
}

/* ---------- actions ---------- */

static int marked(Pane *p) {
    int n = 0;
    for (int i = 0; i < p->n; ++i) n += p->e[i].mark;
    return n;
}

/* The items an action applies to: the marked ones, or the cursor row. */
static int targets(Pane *p, int idx[], int max) {
    int n = 0;
    for (int i = 0; i < p->n && n < max; ++i) if (p->e[i].mark) idx[n++] = i;
    if (!n && p->n) idx[n++] = p->sel;
    return n;
}

static void transfer(int kind) {
    Pane *src = &pane[active], *dst = &pane[!active];
    if (!src->path[0] || !dst->path[0]) {
        ui_message("Pick a folder", "Open a folder in both panes first: items go from this pane into the other one.");
        return;
    }
    static int idx[MAX_OPS];
    int n = targets(src, idx, MAX_OPS), clash = 0;
    for (int i = 0; i < n; ++i) {
        char to[FO_PATH_MAX];
        join(to, sizeof(to), dst->path, src->e[idx[i]].name);
        SceIoStat st;
        clash += sceIoGetstat(to, &st) >= 0;
    }
    char body[300];
    snprintf(body, sizeof(body), "%s %d item%s\nfrom %s\nto %s%s", kind == FO_JOB_COPY ? "Copy" : "Move", n,
             n == 1 ? "" : "s", src->path, dst->path,
             clash ? "\n\nSome already exist there and will be replaced." : "");
    if (!ui_confirm(kind == FO_JOB_COPY ? "Copy" : "Move", body)) return;
    for (int i = 0; i < n; ++i) {
        char from[FO_PATH_MAX], to[FO_PATH_MAX];
        join(from, sizeof(from), src->path, src->e[idx[i]].name);
        join(to, sizeof(to), dst->path, src->e[idx[i]].name);
        queue_op(kind, from, to);
        src->e[idx[i]].mark = 0;
    }
}

static void delete_items(void) {
    Pane *p = &pane[active];
    if (!p->path[0]) return;
    static int idx[MAX_OPS];
    int n = targets(p, idx, MAX_OPS);
    char body[300];
    if (n == 1) snprintf(body, sizeof(body), "Delete %s%s? This cannot be undone.", p->e[idx[0]].name,
                         p->e[idx[0]].dir ? " and everything in it" : "");
    else snprintf(body, sizeof(body), "Delete %d items? This cannot be undone.", n);
    if (!ui_confirm("Delete", body)) return;
    for (int i = 0; i < n; ++i) {
        char path[FO_PATH_MAX];
        join(path, sizeof(path), p->path, p->e[idx[i]].name);
        if (fo_can_remove(path) < 0) {
            char msg[400];
            snprintf(msg, sizeof(msg), "%s is protected: device roots, top-level folders, tai/ and the agent remote cannot be deleted from here.", path);
            ui_message("Protected", msg);
            return;
        }
        queue_op(FO_JOB_REMOVE, path, NULL);
    }
}

static void rename_item(void) {
    Pane *p = &pane[active];
    if (!p->path[0] || !p->n) return;
    char name[256];
    snprintf(name, sizeof(name), "%s", p->e[p->sel].name);
    if (!ui_ask_text("Rename to", name, sizeof(name)) || !name[0] || strchr(name, '/')) return;
    char from[FO_PATH_MAX], to[FO_PATH_MAX];
    join(from, sizeof(from), p->path, p->e[p->sel].name);
    join(to, sizeof(to), p->path, name);
    int rc = fo_rename(from, to);
    if (rc < 0) {
        char msg[200];
        snprintf(msg, sizeof(msg), "Could not rename (0x%08X). Top-level folders and tai/ are protected.", rc);
        ui_message("Rename failed", msg);
    }
    load(p, name);
}

static void new_folder(void) {
    Pane *p = &pane[active];
    if (!p->path[0]) return;
    char name[256] = "New folder";
    if (!ui_ask_text("Folder name", name, sizeof(name)) || !name[0] || strchr(name, '/')) return;
    char path[FO_PATH_MAX];
    join(path, sizeof(path), p->path, name);
    int rc = fo_mkdirs(path);
    if (rc < 0) {
        char msg[160];
        snprintf(msg, sizeof(msg), "Could not create it (0x%08X).", rc);
        ui_message("New folder failed", msg);
    }
    load(p, name);
}

static void properties(int hash) {
    Pane *p = &pane[active];
    if (!p->path[0] || !p->n) return;
    char path[FO_PATH_MAX];
    join(path, sizeof(path), p->path, p->e[p->sel].name);
    if (hash) {
        if (p->e[p->sel].dir) { ui_message("SHA-256", "Pick a file, not a folder."); return; }
        queue_op(FO_JOB_SHA256, path, NULL);
        return;
    }
    SceIoStat st;
    char msg[600], size[32];
    if (sceIoGetstat(path, &st) < 0) return;
    human_size(st.st_size, size, sizeof(size));
    snprintf(msg, sizeof(msg), "%s\n\n%s    %s\nModified %04d-%02d-%02d %02d:%02d UTC", path,
             SCE_S_ISDIR(st.st_mode) ? "Folder" : "File", SCE_S_ISDIR(st.st_mode) ? "" : size,
             st.st_mtime.year, st.st_mtime.month, st.st_mtime.day, st.st_mtime.hour, st.st_mtime.minute);
    ui_message(p->e[p->sel].name, msg);
}

static void action_menu(void) {
    if (files_busy()) {
        static const char *const busy[] = {"Stop the running job", "Keep going"};
        if (ui_menu("A job is running", busy, 2) == 0 &&
            ui_confirm("Stop", "Stop the running job and drop anything queued behind it? A file half-copied is removed.")) {
            op_head = op_tail;                            /* drop what has not started */
            fo_job_cancel();
        }
        return;
    }
    static const char *const items[] = {"Copy to other pane", "Move to other pane", "Delete", "Rename",
                                        "New folder", "SHA-256", "Properties"};
    switch (ui_menu("Actions", items, 7)) {
    case 0: transfer(FO_JOB_COPY); break;
    case 1: transfer(FO_JOB_MOVE); break;
    case 2: delete_items(); break;
    case 3: rename_item(); break;
    case 4: new_folder(); break;
    case 5: properties(1); break;
    case 6: properties(0); break;
    }
}

/* ---------- drawing ---------- */

static void device_line(const char *dev, char *out, int max) {    /* from the reader's cache */
    out[0] = 0;
    for (unsigned int i = 0; i < sizeof(DEVICES) / sizeof(DEVICES[0]); ++i)
        if (!strcmp(DEVICES[i], dev)) { snprintf(out, max, "%s", dev_line_cache[i]); return; }
}

static void draw_pane(int i, int x) {
    Pane *p = &pane[i];
    int on = i == active;
    vita2d_draw_rectangle(x, 76, PANE_W, 382, C_PANEL);
    if (on) vita2d_draw_rectangle(x, 76, PANE_W, 2, C_ACCENT);
    text_fit(bold, x + 16, 101, on ? C_TEXT : C_DIM, 18, p->path[0] ? p->path : "Devices", PANE_W - 90);
    char count[32];
    int m = marked(p);
    snprintf(count, sizeof(count), m ? "%d marked" : "%d", m ? m : p->n);
    text_right(font, x + PANE_W - 14, 101, m ? C_MARK : C_FAINT, 16, count);

    float target = p->sel < p->top + 2 ? p->sel - 2 : p->sel > p->top + ROWS - 3 ? p->sel - ROWS + 3 : p->top;
    if (target > p->n - ROWS) target = p->n - ROWS;
    if (target < 0) target = 0;
    p->top += (target - p->top) * 0.35f;
    int first = (int)p->top;
    float frac = p->top - first;
    for (int r = -1; r <= ROWS; ++r) {
        int idx = first + r;
        if (idx < 0 || idx >= p->n) continue;
        float y = LIST_Y + (r - frac) * ROW_H;
        if (y < LIST_Y - 2 || y > LIST_Y + ROWS * ROW_H - ROW_H + 2) continue;
        Entry *e = &p->e[idx];
        if (idx == p->sel) vita2d_draw_rectangle(x + 6, y, PANE_W - 12, ROW_H - 2, on ? C_SEL : C_SEL_DIM);
        if (e->mark) vita2d_draw_rectangle(x + 6, y, 4, ROW_H - 2, C_MARK);
        text(bold, x + 18, y + 23, e->dir ? C_ACCENT : C_FAINT, 15, e->dir ? ">" : "-");
        text_fit(font, x + 36, y + 23, e->mark ? C_MARK : C_TEXT, 18, e->name, PANE_W - 170);
        char right[48];
        if (!p->path[0]) device_line(e->name, right, sizeof(right));
        else if (e->dir) right[0] = 0;
        else human_size(e->size, right, sizeof(right));
        text_right(font, x + PANE_W - 14, y + 23, C_DIM, 16, right);
    }
    if (p->loading && !p->n) { draw_shimmer(x + 16, LIST_Y + 4, PANE_W - 32, 24); text(font, x + 36, LIST_Y + 60, C_FAINT, 16, "Loading\xE2\x80\xA6"); }
    else if (!p->n) text(font, x + 36, LIST_Y + 24, C_FAINT, 18, p->path[0] ? "Empty folder" : "No devices");
}

static void draw_job(void) {
    FoJobInfo j;
    fo_job_info(&j);
    if (j.state != FO_ST_RUNNING && op_head == op_tail) return;
    static const char *const kinds[] = {"", "Copying", "Moving", "Deleting", "Hashing", "Downloading"};
    char line[200], a[32], b[32];
    human_size(j.bytes_done, a, sizeof(a));
    human_size(j.bytes_total, b, sizeof(b));
    int queued = op_tail - op_head;
    if (j.state == FO_ST_RUNNING && j.phase == 0) snprintf(line, sizeof(line), "%s: measuring...", kinds[j.kind]);
    else snprintf(line, sizeof(line), "%s  %u of %u files   %s of %s%s", kinds[j.kind], j.files_done,
                  j.files_total, a, b, queued ? "   (more queued)" : "");
    text_fit(font, 18, 486, C_TEXT, 16, line, 570);
    draw_bar(600, 476, 250, 10, j.bytes_total ? (float)j.bytes_done / j.bytes_total : 0, C_ACCENT);
    draw_hints(862, 481, "/\\ stop", C_DIM, W);
}

static void show_hash_result(void) {
    static int shown;
    FoJobInfo j;
    fo_job_info(&j);
    if (j.kind == FO_JOB_SHA256 && j.state == FO_ST_DONE && j.id != shown && op_head == op_tail) {
        shown = j.id;
        char msg[160];
        snprintf(msg, sizeof(msg), "%.32s\n%.32s", j.note, j.note + 32);
        ui_message("SHA-256", msg);
    }
}

const char *files_hint(void) {
    return "X open   O back   [] mark   /\\ actions   <- -> pane   START devices   L R tabs";
}

void files_update(const Input *in) {
    take_loaded(&pane[0]);
    take_loaded(&pane[1]);
    Pane *p = &pane[active];
    if (in->pressed & SCE_CTRL_UP && p->n) p->sel = (p->sel + p->n - 1) % p->n;
    if (in->pressed & SCE_CTRL_DOWN && p->n) p->sel = (p->sel + 1) % p->n;
    /* L/R switch Home's tabs, so the D-pad picks the pane. */
    if (in->pressed & SCE_CTRL_LEFT) active = 0;
    if (in->pressed & SCE_CTRL_RIGHT) active = 1;
    if (in->pressed & SCE_CTRL_CROSS) enter(p);
    if (in->pressed & SCE_CTRL_CIRCLE) go_up(p);
    if (in->pressed & SCE_CTRL_SQUARE && p->n && p->path[0]) {
        p->e[p->sel].mark ^= 1;
        if (p->sel < p->n - 1) ++p->sel;
    }
    if (in->pressed & SCE_CTRL_START) { p->path[0] = 0; load(p, NULL); }
    if (in->pressed & SCE_CTRL_TRIANGLE) action_menu();

    /* Touch: tap a row to select it (tap again to open), drag to scroll. */
    if (in->tapped && in->tap_y > LIST_Y && in->tap_y < LIST_Y + ROWS * ROW_H) {
        int side = in->tap_x >= W / 2;
        Pane *t = &pane[side];
        int idx = (int)(t->top + (in->tap_y - LIST_Y) / (float)ROW_H);
        if (idx >= 0 && idx < t->n) {
            if (side == active && idx == t->sel) enter(t);
            else { active = side; t->sel = idx; }
        }
    } else if (in->tapped && in->tap_y > 76 && in->tap_y < LIST_Y) {
        active = in->tap_x >= W / 2;                      /* tapping a pane's path goes up */
        go_up(&pane[active]);
    }
    if (in->touching && in->drag_dy) {
        Pane *t = &pane[in->tx >= W / 2];
        t->top -= in->drag_dy / (float)ROW_H;
        if (t->top < 0) t->top = 0;
        int mid = (int)(t->top + ROWS / 2);
        if (mid < t->n) t->sel = mid;
    }

    pump_ops();
    if (reload_after) { reload(&pane[0]); reload(&pane[1]); reload_after = 0; }

    draw_pane(0, 12);
    draw_pane(1, W - 12 - PANE_W);
    draw_job();
    show_hash_result();
}
