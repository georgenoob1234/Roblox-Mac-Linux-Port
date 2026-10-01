// Darling's directory service sometimes loses the current user's passwd entry.
// Preserve its records; reconstruct only the running guest user's missing entry
// from the session environment that Darling itself established at launch.
#include <pwd.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dlfcn.h>
extern void dispatch_once_f(long *,void *,void (*)(void *));
static struct passwd *(*lookup)(uid_t);
static int (*lookup_r)(uid_t,struct passwd *,char *,size_t,struct passwd **);
static long once;
static void load(void *unused) {
    (void)unused;lookup=dlsym(RTLD_NEXT,"getpwuid");lookup_r=dlsym(RTLD_NEXT,"getpwuid_r");
}
static int session_entry(uid_t uid,struct passwd *record,char *buffer,size_t size,struct passwd **result) {
    *result=NULL;
    const char *home=getenv("HOME"),*name=getenv("USER");
    if(!name || !*name)name=getenv("LOGNAME");
    if(uid!=getuid() || !home || home[0]!='/' || !name || !*name)return ENOENT;
    const char *fields[]={name,"*","",name,home,"/bin/bash"};
    size_t needed=0;for(unsigned i=0;i<6;i++)needed+=strlen(fields[i])+1;
    if(size<needed)return ERANGE;
    memset(record,0,sizeof(*record));record->pw_uid=uid;record->pw_gid=getgid();
    char **dest[]={&record->pw_name,&record->pw_passwd,&record->pw_class,&record->pw_gecos,&record->pw_dir,&record->pw_shell};
    for(unsigned i=0;i<6;i++){size_t n=strlen(fields[i])+1;memcpy(buffer,fields[i],n);*dest[i]=buffer;buffer+=n;}
    *result=record;return 0;
}
struct passwd *getpwuid(uid_t uid) {
    dispatch_once_f(&once,NULL,load);
    struct passwd *record=lookup?lookup(uid):NULL;
    if(record && record->pw_dir)return record;
    static _Thread_local struct passwd local;
    static _Thread_local char buffer[16384];
    int saved=errno,rc=session_entry(uid,&local,buffer,sizeof(buffer),&record);
    errno=rc==ENOENT?saved:rc;return rc?NULL:record;
}
int getpwuid_r(uid_t uid,struct passwd *record,char *buffer,size_t size,struct passwd **result) {
    if(!record || !buffer || !result)return EINVAL;
    dispatch_once_f(&once,NULL,load);*result=NULL;
    int rc=lookup_r?lookup_r(uid,record,buffer,size,result):0;
    if(*result && (*result)->pw_dir)return rc;
    if(rc==ERANGE)return rc;
    int fallback=session_entry(uid,record,buffer,size,result);
    return fallback==ENOENT?rc:fallback;
}
