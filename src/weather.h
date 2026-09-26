#ifndef HOME_WEATHER_H
#define HOME_WEATHER_H

void weather_init(void);                       /* after the network is up */
int weather_line(char *out, int max, char *sub, int submax, int *kind);   /* 0 until the first answer; kind: 0 sun 1 cloud 2 rain 3 snow */

int weather_search(const char *query, char names[][96], float *lat, float *lon, int *us, int max);  /* blocking; -1 no network */
void weather_set(const char *name, float lat, float lon, int fahrenheit);
void weather_off(void);
const char *weather_place(void);   /* "" when none is set */

#endif
