#include <mach-o/loader.h>
#include <stdint.h>
struct GuestClass {uintptr_t isa,superclass,cache[2],bits;};
struct GuestRO {uint32_t flags,start,size,reserved;uintptr_t ivars,name,methods,protocols,ivarlist,weak,properties;};
struct GuestMethods {uint32_t flags,count;};
struct NativeMethod {SEL name;const char *types;IMP imp;};
struct NativeMethods {uint32_t flags,count;struct NativeMethod methods[];};
static const struct mach_header_64 *preparing_header;
static intptr_t preparing_slide;
static unsigned deferred_methods;
struct GuestMethodBridge {IMP native;uint64_t guest;};
static struct GuestMethodBridge *guest_methods;
static size_t guest_method_count,guest_method_capacity;
static int compare_guest_method(const void *a,const void *b){
    uintptr_t x=(uintptr_t)((const struct GuestMethodBridge*)a)->native,y=(uintptr_t)((const struct GuestMethodBridge*)b)->native;
    return (x>y)-(x<y);
}
static uint64_t guest_method(IMP native){
    struct GuestMethodBridge key={native,0};
    struct GuestMethodBridge *found=bsearch(&key,guest_methods,guest_method_count,sizeof(key),compare_guest_method);
    return found?found->guest:0;
}
static int unavailable_method(armrt_cpu *cpu,void *message){return p_armrt_fail(cpu,message);}
static int in_guest(const void *pointer,size_t size) {
    uintptr_t address=(uintptr_t)pointer;
    const struct load_command *lc=(const void*)(preparing_header+1);
    for(unsigned i=0;i<preparing_header->ncmds;++i,lc=(const void*)((const char*)lc+lc->cmdsize)) {
        if(lc->cmd!=LC_SEGMENT_64)continue;
        const struct segment_command_64 *seg=(const void*)lc;
        if(!seg->initprot)continue;
        uintptr_t begin=seg->vmaddr+preparing_slide;
        if(address>=begin && address-begin<=seg->vmsize && size<=seg->vmsize-(address-begin))return 1;
    }
    return 0;
}
static int guest_string(const char *p) {
    for(unsigned i=0;i<4096;++i){if(!in_guest(p+i,1))return 0;if(!p[i])return 1;}
    return 0;
}
static int convert_methods(armrt_cpu *cpu,uintptr_t *slot) {
    if(!*slot)return 0;
    struct GuestMethods *list=(void*)*slot;
    if(!in_guest(list,sizeof(*list)) || list->count>65536)return -1;
    unsigned small=(list->flags&0x80000000)!=0,stride=list->flags&0xffff;
    stride&=~3u;
    if(stride!=(small?12:24) || !in_guest(list,8+(size_t)stride*list->count))return -1;
    struct NativeMethods *native=calloc(1,8+(size_t)list->count*sizeof(struct NativeMethod));
    if(!native)return -1;
    native->flags=24;native->count=list->count;
    for(unsigned i=0;i<list->count;++i) {
        char *entry=(char*)(list+1)+(size_t)i*stride;
        const char *name,*types;uintptr_t imp;
        if(small) {
            int32_t *r=(void*)entry;
            const char *selector=(const char*)&r[0]+r[0];
            if(list->flags&0x40000000)name=selector;
            else {if(!in_guest(selector,8)){free(native);return -1;}name=*(const char**)selector;}
            types=(const char*)&r[1]+r[1];imp=(uintptr_t)&r[2]+r[2];
        }else {
            uintptr_t *r=(void*)entry;name=(const char*)r[0];types=(const char*)r[1];imp=r[2];
        }
        if(!guest_string(name) || !guest_string(types) || !in_guest((void*)imp,4)){free(native);return -1;}
        uint64_t original_imp=imp;
        char signature[2048];
        if(encode_signature(types,signature,sizeof(signature))) {
            // Block parameters have pointer calling conventions, but their
            // invoke/copy/dispose functions need adapters. Register an explicit
            // failure IMP for such methods until those adapters exist; never
            // call a raw ARM block from native code or silently drop a callback.
            char fixed[2048];size_t length=strlen(types),out=0;
            if(length>=sizeof(fixed)){free(native);return -1;}
            for(size_t n=0;n<length;++n){fixed[out++]=types[n];if(types[n]=='@' && types[n+1]=='?')++n;}
            fixed[out]=0;
            if(encode_signature(fixed,signature,sizeof(signature))){fprintf(stderr,"Track B metadata: unsupported method %s (%s)\n",name,types);free(native);return -1;}
            char *message=NULL;asprintf(&message,"Objective-C block method not implemented: %s",name);
            if(!message){free(native);return -1;}
            imp=p_armrt_bind_handler(cpu,unavailable_method,message);++deferred_methods;
        }
        void *callback=p_armrt_callback(cpu,imp,signature);
        if(!callback){fprintf(stderr,"Track B metadata: %s: %s\n",name,p_armrt_error(cpu));free(native);return -1;}
        if(guest_method_count==guest_method_capacity){
            size_t capacity=guest_method_capacity?guest_method_capacity*2:128;
            void *grown=realloc(guest_methods,capacity*sizeof(*guest_methods));
            if(!grown){free(native);return -1;}
            guest_methods=grown;guest_method_capacity=capacity;
        }
        guest_methods[guest_method_count++]=(struct GuestMethodBridge){(IMP)callback,original_imp};
        native->methods[i]=(struct NativeMethod){sel_registerName(name),types,(IMP)callback};
    }
    *slot=(uintptr_t)native;return 0;
}
static int convert_class(armrt_cpu *cpu,uintptr_t pointer) {
    struct GuestClass *cls=(void*)pointer;
    if(!in_guest(cls,sizeof(*cls)))return -1;
    struct GuestRO *ro=(void*)(cls->bits&~(uintptr_t)7);
    if(!in_guest(ro,sizeof(*ro)))return -1;
    return convert_methods(cpu,&ro->methods);
}
static int prepare_objc(armrt_cpu *cpu,void *header,int64_t slide,const char *path,void *user) {
    (void)user;preparing_header=header;preparing_slide=slide;deferred_methods=0;
    unsigned classes=0,categories=0;
    const struct load_command *lc=(const void*)(preparing_header+1);
    for(unsigned i=0;i<preparing_header->ncmds;++i,lc=(const void*)((const char*)lc+lc->cmdsize)) {
        if(lc->cmd!=LC_SEGMENT_64)continue;
        const struct segment_command_64 *seg=(const void*)lc;
        const struct section_64 *sections=(const void*)(seg+1);
        for(unsigned n=0;n<seg->nsects;++n) {
            const struct section_64 *section=&sections[n];uintptr_t *items=(void*)(section->addr+slide);
            if(!strncmp(section->sectname,"__objc_classlist",16)) {
                if(section->size%8 || !in_guest(items,section->size))return -1;
                for(unsigned k=0;k<section->size/8;++k) {
                    if(convert_class(cpu,items[k]))return -1;
                    if(convert_class(cpu,((struct GuestClass*)items[k])->isa))return -1;
                    ++classes;
                }
            }else if(!strncmp(section->sectname,"__objc_catlist",16)) {
                if(section->size%8 || !in_guest(items,section->size))return -1;
                for(unsigned k=0;k<section->size/8;++k) {
                    uintptr_t *cat=(void*)items[k];if(!in_guest(cat,6*8))return -1;
                    if(convert_methods(cpu,&cat[2]) || convert_methods(cpu,&cat[3]))return -1;
                    ++categories;
                }
            }
        }
    }
    void (*map)(unsigned,const char **,const struct mach_header **)=dlsym(RTLD_DEFAULT,"map_images");
    void (*load)(const char *,const struct mach_header *)=dlsym(RTLD_DEFAULT,"load_images");
    if(!map || !load)return -1;
    qsort(guest_methods,guest_method_count,sizeof(*guest_methods),compare_guest_method);
    const struct mach_header *mh=header;map(1,&path,&mh);load(path,mh);
    fprintf(stderr,"Track B metadata: registered %u ARM classes and %u categories (%u block methods fail explicitly if invoked)\n",classes,categories,deferred_methods);
    return 0;
}
