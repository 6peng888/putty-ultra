/*
 * Portable implementation of ltime() for any ISO-C platform where
 * time_t behaves. (In practice, we've found that platforms such as
 * Windows and Mac have needed their own specialised implementations.)
 */

#include <time.h>
#include <assert.h>
#include <sys/time.h>

struct tm ltime(void)
{
    time_t t;
    time(&t);
    assert (t != ((time_t)-1));
    return *localtime(&t);
}

int ltime_milliseconds(void)
{
    struct timeval tv;

    if (gettimeofday(&tv, NULL) != 0)
        return 0;
    return (int)(tv.tv_usec / 1000);
}
