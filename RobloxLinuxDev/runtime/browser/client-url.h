#pragma once
#include <string.h>
#include <strings.h>
static int rbx_client_url(const char *url) {
    if(!url || strlen(url)>16384)return 0;
    for(const unsigned char *p=(const unsigned char*)url;*p;p++)if(*p<32 || *p==127)return 0;
    return (!strncasecmp(url,"roblox:",7) && url[7]) ||
           (!strncasecmp(url,"roblox-player:",14) && url[14]);
}
