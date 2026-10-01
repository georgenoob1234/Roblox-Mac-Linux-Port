#pragma once
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
// Convert runtime encodings into armrt's fixed ABI signatures. Unsupported
// callback/block/vector/union/bitfield cases fail instead of guessing an ABI.
static int skip_pointee(const char **input,unsigned depth) {
    if(depth>16)return -1;
    const char *p=*input;while(*p && strchr("rnNoORV",*p))++p;
    char c=*p++;if(!c)return -1;
    if(c=='^'){if(skip_pointee(&p,depth+1))return -1;}
    else if(c=='{' || c=='(' || c=='[') {
        char close=c=='{'?'}':c=='('?')':']';unsigned nesting=1;
        while(*p && nesting) {
            if(*p=='"'){++p;while(*p && *p!='"')++p;if(!*p)return -1;}
            else if(*p==c)++nesting;else if(*p==close)--nesting;
            ++p;
        }
        if(nesting)return -1;
    }else if(c=='@') {
        if(*p=='?')++p;
        else if(*p=='"'){++p;while(*p && *p!='"')++p;if(*p++!='"')return -1;}
    }else if(!strchr("?#:*vfdqQlLiIsScCB",c))return -1;
    *input=p;return 0;
}
static int encode_type(const char **input,char **output,char *end,unsigned depth) {
    if(depth>8 || *output>=end)return -1;
    const char *p=*input;while(*p && strchr("rnNoORV",*p))++p;
    char c=*p++;if(!c)return -1;
    if(c=='^') {
        if(*p=='?')return -1;
        // Pointee layout does not affect pointer passing. Consume the encoding.
        if(skip_pointee(&p,depth+1))return -1;
        *(*output)++='p';
    }else if(c=='{') {
        // libc++ shared_ptr has a non-trivial copy/destructor: both target ABIs
        // pass its by-value parameter using an invisible pointer. The encoding
        // records its fields but omits this C++ calling-convention distinction.
        if(!strncmp(p,"shared_ptr<",11) || !strncmp(p,"function<",9) || !strncmp(p,"basic_string<",13)) {
            const char *body=p;unsigned braces=1;
            while(*body && braces){if(*body=='{')++braces;else if(*body=='}')--braces;++body;}
            if(braces)return -1;
            *(*output)++='p';*input=body;return 0;
        }
        *(*output)++='{';
        while(*p && *p!='=' && *p!='}')++p;
        if(*p++!='=')return -1;
        while(*p && *p!='}') {
            if(*p=='"'){++p;while(*p && *p!='"')++p;if(*p++!='"')return -1;}
            if(encode_type(&p,output,end,depth+1))return -1;
        }
        if(*p++!='}' || *output>=end)return -1;
        *(*output)++='}';
    }else if(c=='[') {
        char *next;unsigned long n=strtoul(p,&next,10);p=next;
        if(!n || n>64)return -1;
        char element[2048],*e=element;
        if(encode_type(&p,&e,element+sizeof(element)-1,depth+1) || *p++!=']')return -1;
        size_t bytes=e-element;
        if((size_t)(end-*output)<n*bytes)return -1;
        while(n--){memcpy(*output,element,bytes);*output+=bytes;}
    }else {
        char t=0;
        switch(c) {
        case '@':if(*p=='?')return -1;if(*p=='"'){++p;while(*p && *p!='"')++p;if(*p++!='"')return -1;}t='p';break;
        case '#':case ':':case '*':t='p';break;
        case 'q':case 'Q':case 'l':case 'L':t='q';break;
        case 'i':t='i';break;case 'I':t='u';break;
        case 's':t='h';break;case 'S':t='H';break;
        case 'c':t='b';break;case 'C':case 'B':t='B';break;
        case 'v':case 'f':case 'd':t=c;break;
        default:return -1;
        }
        *(*output)++=t;
    }
    *input=p;return 0;
}
static int encode_signature(const char *encoding,char *output,size_t capacity) {
    if(!capacity)return -1;
    char *out=output,*end=output+capacity-1;
    const char *cxx_return=!strncmp(encoding,"{shared_ptr<",12)?"!{pp}":
        !strncmp(encoding,"{function<",10)?"!{qqqq}":!strncmp(encoding,"{basic_string<",14)?"!{qqq}":NULL;
    if(cxx_return) {
        char ignored[2048],*p=ignored;
        if(encode_type(&encoding,&p,ignored+sizeof(ignored)-1,0))return -1;
        size_t length=strlen(cxx_return);if((size_t)(end-out)<length)return -1;
        memcpy(out,cxx_return,length);out+=length;
        while(isdigit((unsigned char)*encoding))++encoding;
    }
    while(*encoding) {
        if(encode_type(&encoding,&out,end,0))return -1;
        while(isdigit((unsigned char)*encoding))++encoding;
    }
    *out=0;return 0;
}

static const char *cached_signature(const char *encoding,char fallback[2048]) {
    // Cache ABI conversion only. IMP lookup still runs for each message, and
    // comparing the encoding bytes handles address reuse or changed metadata.
    static __thread struct {char source[512],signature[2048];} cache[64];
    uintptr_t key=(uintptr_t)encoding;
    unsigned index=((key>>4)^(key>>10))&63;
    if(cache[index].source[0] && !strcmp(cache[index].source,encoding))return cache[index].signature;
    if(encode_signature(encoding,fallback,2048))return NULL;
    if(strlen(encoding)>=sizeof(cache[index].source))return fallback;
    strcpy(cache[index].source,encoding);strcpy(cache[index].signature,fallback);
    return cache[index].signature;
}
