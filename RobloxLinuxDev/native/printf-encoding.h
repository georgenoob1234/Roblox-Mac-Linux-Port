#pragma once
#include <string.h>
/* Only ABI classification: libc still formats the text. Reject formats that
 * need positional reordering or different native types instead of guessing. */
static int format_types(const char *format,char *out,size_t capacity,int objc){
    size_t count=0;
    if(!format || !capacity)return -1;
    size_t length=0;while(length<65536 && format[length])++length;if(length==65536)return -1;
#define ARG(c) do{if(count+1>=capacity)return -1;out[count++]=(c);}while(0)
    while(*format){
        if(*format++!='%')continue;
        if(*format=='%'){++format;continue;}
        while(*format && strchr("#0- +'",*format))++format;
        if(*format=='*'){ARG('i');++format;}
        else while(*format>='0' && *format<='9')++format;
        if(*format=='.'){
            ++format;if(*format=='*'){ARG('i');++format;}
            else while(*format>='0' && *format<='9')++format;
        }
        char length=0;
        if(*format && strchr("hljztqL",*format)){
            length=*format++;if((length=='h' || length=='l') && *format==length)++format;
        }
        char conversion=*format;if(!conversion)return -1;++format;
        switch(conversion){
        case 'd':case 'i':case 'o':case 'u':case 'x':case 'X':
            if(length=='L')return -1;ARG(length && length!='h'?'q':conversion=='i'||conversion=='d'?'i':'u');break;
        case 'a':case 'A':case 'e':case 'E':case 'f':case 'F':case 'g':case 'G':
            if(length && length!='l')return -1;ARG('d');break;
        case 'c':if(length && length!='l')return -1;ARG('i');break;
        case 's':if(length && length!='l')return -1;ARG('p');break;
        case 'p':if(length)return -1;ARG('p');break;
        case 'n':if(length=='L')return -1;ARG('p');break;
        case '@':if(!objc || length)return -1;ARG('p');break;
        default:return -1;
        }
    }
    out[count]=0;return 0;
#undef ARG
}
static int printf_types(const char *format,char *out,size_t capacity){return format_types(format,out,capacity,0);}
static int scanf_types(const char *format,char *out,size_t capacity){
    size_t count=0;if(!format || !capacity)return -1;
    size_t length=0;while(length<65536 && format[length])++length;if(length==65536)return -1;
    while(*format){
        if(*format++!='%')continue;
        if(*format=='%'){++format;continue;}
        int suppressed=*format=='*';if(suppressed)++format;
        while(*format>='0' && *format<='9')++format;
        char length=0;if(*format && strchr("hljzt",*format)){length=*format++;if((length=='h'||length=='l') && *format==length)++format;}
        char conversion=*format;if(!conversion)return -1;++format;
        if(conversion=='['){
            if(*format=='^')++format;if(*format==']')++format;
            while(*format && *format!=']')++format;if(!*format)return -1;++format;
        }else if(!strchr("diouxXaAeEfFgGcspn",conversion))return -1;
        if(!suppressed){if(count+1>=capacity)return -1;out[count++]='p';}
    }
    out[count]=0;return 0;
}
