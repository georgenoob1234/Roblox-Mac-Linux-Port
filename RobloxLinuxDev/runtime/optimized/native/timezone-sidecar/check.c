#include <assert.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
static const char *paris = "/usr/share/zoneinfo/Europe/Paris",
                  *utc = "/usr/share/zoneinfo/UTC";
static const char *defaultfile = "/Volumes/SystemRoot/mnt/default-zone",
                  *namedfile = "/Volumes/SystemRoot/mnt/named-zone";
static const time_t winter = 1767268800, summer = 1782907200,
                    historical = -2208988800LL;
static int done;
static void copyzone(const char *source, const char *target, time_t stamp,
                     int replace) {
    char temporary[1024];
    assert(snprintf(temporary, sizeof(temporary), "%s.new", target) <
           sizeof(temporary));
    const char *out = replace ? temporary : target;
    FILE *in = fopen(source, "rb"), *dest = fopen(out, "wb");
    assert(in && dest);
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)))
        assert(fwrite(buf, 1, n, dest) == n);
    assert(!ferror(in));
    assert(!fclose(in));
    assert(!fclose(dest));
    struct timeval times[2] = {{stamp, 123456}, {stamp, 123456}};
    assert(!utimes(out, times));
    if (replace)
        assert(!rename(out, target));
}
static void offset(time_t when, long expected) {
    struct tm tm;
    char formatted[100];
    assert(localtime_r(&when, &tm));
    if (tm.tm_gmtoff != expected) {
        fprintf(stderr, "offset mismatch TZ=%s expected=%ld actual=%ld\n",
                getenv("TZ"), expected, tm.tm_gmtoff);
        abort();
    }
    assert(strftime(formatted, sizeof(formatted), "%Y-%m-%d %H:%M:%S %z", &tm));
}
static void zone(const char *tz, long win, long sum, long historic) {
    if (tz)
        assert(!setenv("TZ", tz, 1));
    else
        assert(!unsetenv("TZ"));
    offset(winter, win);
    offset(summer, sum);
    offset(historical, historic);
}
static void *reader(void *unused) {
    (void)unused;
    while (!__atomic_load_n(&done, __ATOMIC_SEQ_CST)) {
        struct tm tm;
        assert(localtime_r(&winter, &tm));
        assert(tm.tm_year == 126 && tm.tm_mon == 0 && tm.tm_mday == 1);
        assert((tm.tm_gmtoff == 0 && tm.tm_hour == 12) ||
               (tm.tm_gmtoff == 3600 && tm.tm_hour == 13));
    }
    return NULL;
}
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    Dl_info info;
    assert(dladdr((void *)localtime_r, &info));
    printf("libc=%s\n", info.dli_fname);
    assert(getenv("TZ_CHECK_LIBRARY") &&
           !strcmp(info.dli_fname, getenv("TZ_CHECK_LIBRARY")));
    assert(!unlink(defaultfile));
    zone(NULL, 0, 0, 0);
    copyzone(paris, defaultfile, 1700000000, 1);
    zone(NULL, 3600, 7200, 561);
    zone(":/usr/share/zoneinfo/Europe/Paris", 3600, 7200, 561);
    zone("UTC0", 0, 0, 0);
    zone("", 0, 0, 0);
    zone("CET-1CEST,M3.5.0,M10.5.0/3", 3600, 7200, 3600);
    char longtz[302];
    memset(longtz, 'Q', 300);
    longtz[300] = '0';
    longtz[301] = 0;
    zone(longtz, 0, 0, 0);
    zone(":/usr/share/zoneinfo/Europe/Paris", 3600, 7200, 561);
    puts("PASS DST, historical Paris offset, UTC, empty/long TZ, repeated "
         "environment switches");
    assert(!unsetenv("TZ"));
    offset(winter, 3600);
    copyzone(utc, defaultfile, 1700000000, 1);
    offset(winter, 0);
    copyzone(paris, defaultfile, 1600000000, 0);
    offset(winter, 3600);
    assert(!unlink(defaultfile));
    offset(winter, 0);
    copyzone(paris, defaultfile, 1500000000, 1);
    offset(winter, 3600);
    puts("PASS resolved default replacement equal mtime, in-place older mtime, "
         "missing file retry");
    unlink(namedfile);
    assert(!setenv("TZ", ":/Volumes/SystemRoot/mnt/named-zone", 1));
    offset(winter, 0);
    copyzone(paris, namedfile, 1700000000, 1);
    offset(winter, 3600);
    offset(historical, 561);
    copyzone(utc, namedfile, 1700000000, 1);
    offset(winter, 0);
    copyzone(paris, namedfile, 1600000000, 0);
    offset(winter, 3600);
    puts("PASS explicit TZ file appearance/replacement without changing TZ, "
         "equal and older mtimes");
    assert(!unsetenv("TZ"));
    offset(winter, 3600);
    pthread_t threads[4];
    for (int i = 0; i < 4; ++i)
        assert(!pthread_create(&threads[i], NULL, reader, NULL));
    for (int i = 0; i < 30; ++i) {
        copyzone((i & 1) ? paris : utc, defaultfile, 1500000000 - i, 1);
        usleep(1000);
    }
    __atomic_store_n(&done, 1, __ATOMIC_SEQ_CST);
    for (int i = 0; i < 4; ++i)
        assert(!pthread_join(threads[i], NULL));
    copyzone(paris, defaultfile, 1700000000, 1);
    offset(winter, 3600);
    puts("PASS concurrent conversion and atomic default zone replacement");
    return 0;
}
