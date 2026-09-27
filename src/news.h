#ifndef HOME_NEWS_H
#define HOME_NEWS_H
/* Community news: r/vitahacks' hot posts (Reddit's RSS), at most one fetch an
 * hour, cached on the card. */
typedef struct { char title[200], author[40], summary[700], id[24], image[320], image_path[80]; unsigned int age_s; } NewsItem;
/* image_path is set once the post's picture is on the card (a JPEG); "" until then. */
void news_init(void);                 /* after the network is up */
int news_count(void);
const NewsItem *news_get(int i);
int news_unseen(void);                /* 1 when the top item has not been looked at yet */
void news_mark_seen(void);
#endif
