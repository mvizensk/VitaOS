#ifndef HOME_PLAYTIME_H
#define HOME_PLAYTIME_H

/* Play time per game. Home can't time a game while it runs (it exits for the
 * launch), so it brackets the gap instead: play.c drops a marker when it
 * launches something, and the next time Home starts, whatever is still
 * marked just finished. */

/* Call once from main, after play_init(): closes out a session left running
 * by the last launch (if Home is only just starting, there is nothing to do). */
void playtime_init(void);

/* Call right after a successful launch, from play.c's single choke point
 * (note_played). sys is the system id (e.g. "psp"), not its display name. */
void playtime_record_launch(const char *sys, const char *title);

long playtime_total_seconds(const char *sys, const char *title);   /* 0 if never played */
long playtime_last_played(const char *sys, const char *title);     /* unix time, 0 if never */

/* The Home tab's "Your week" card: hours played in the last 7 days, the top
 * games by time, and the day streak (consecutive days, ending today, with
 * any play at all). Returns 0 (nothing to show) when there was no play in
 * that window. */
#define PLAYTIME_TOP 3
typedef struct { char title[160]; long seconds; } PlaytimeTop;
int playtime_week(long *seconds_out, PlaytimeTop *top_out, int *ntop_out, int *streak_out);

#endif
