#pragma once
#include <stddef.h>
int trackb_sysctl(int *,unsigned,void *,size_t *,void *,size_t);
int trackb___sysctl(int *,unsigned,void *,size_t *,void *,size_t);
int trackb_sysctlbyname(const char *,void *,size_t *,void *,size_t);
int trackb___sysctlbyname(const char *,size_t,void *,size_t *,void *,size_t);
long trackb_sysconf(int);
