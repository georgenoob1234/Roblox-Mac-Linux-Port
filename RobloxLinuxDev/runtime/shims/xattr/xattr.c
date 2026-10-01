// Darling's extended attributes do not work — setxattr/getxattr fail on every path, not just on
// the prefix's overlayfs. Roblox's Crashpad reporter uses them to mark its database initialised
// and refuses to start without them ("Failed to initialize crash reporter"), which is fatal.
//
// Roblox only ever touches three entry points and stores a handful of tiny values, so an
// in-process table is enough. ponytail: not persisted across runs, so Crashpad re-initialises
// its database every launch — harmless. Back it with real files if a crash upload ever needs it.
extern int *__error(void);
extern int strcmp(const char *, const char *);
extern char *strncpy(char *, const char *, unsigned long);
extern void *memcpy(void *, const void *, unsigned long);
extern int pthread_mutex_lock(void *);
extern int pthread_mutex_unlock(void *);

#define ENOATTR 93
#define ERANGE  34
#define ENOSPC  28

#define MAX_ATTRS 64
#define MAX_PATH  512
#define MAX_NAME  128
#define MAX_VALUE 1024

static struct {
    char path[MAX_PATH], name[MAX_NAME];
    unsigned char value[MAX_VALUE];
    unsigned long size;
    int used;
} table[MAX_ATTRS];

static long lock[8];   // pthread_mutex_t is 64 bytes on Darwin; zeroed == PTHREAD_MUTEX_INITIALIZER

static int find(const char *path, const char *name) {
    for (int i = 0; i < MAX_ATTRS; i++)
        if (table[i].used && !strcmp(table[i].path, path) && !strcmp(table[i].name, name))
            return i;
    return -1;
}

long getxattr(const char *path, const char *name, void *value, unsigned long size,
              unsigned position, int options) {
    (void)position; (void)options;
    pthread_mutex_lock(lock);
    int i = find(path, name);
    long r;
    if (i < 0) {
        *__error() = ENOATTR;
        r = -1;
    } else if (size == 0) {
        r = (long)table[i].size;                  // size query
    } else if (size < table[i].size) {
        *__error() = ERANGE;
        r = -1;
    } else {
        memcpy(value, table[i].value, table[i].size);
        r = (long)table[i].size;
    }
    pthread_mutex_unlock(lock);
    return r;
}

int setxattr(const char *path, const char *name, const void *value, unsigned long size,
             unsigned position, int options) {
    (void)position; (void)options;
    if (size > MAX_VALUE) { *__error() = ENOSPC; return -1; }
    pthread_mutex_lock(lock);
    int i = find(path, name);
    if (i < 0)
        for (i = 0; i < MAX_ATTRS && table[i].used; i++) ;
    int r = 0;
    if (i >= MAX_ATTRS) {
        *__error() = ENOSPC;
        r = -1;
    } else {
        strncpy(table[i].path, path, MAX_PATH - 1);
        strncpy(table[i].name, name, MAX_NAME - 1);
        memcpy(table[i].value, value, size);
        table[i].size = size;
        table[i].used = 1;
    }
    pthread_mutex_unlock(lock);
    return r;
}

int removexattr(const char *path, const char *name, int options) {
    (void)options;
    pthread_mutex_lock(lock);
    int i = find(path, name), r = 0;
    if (i < 0) { *__error() = ENOATTR; r = -1; } else table[i].used = 0;
    pthread_mutex_unlock(lock);
    return r;
}
