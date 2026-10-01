// These are our native Metal implementation classes. Their method ABI is fixed
// by the Metal contract; the current IMP is still looked up on every message.
// Arbitrary guest/AppKit classes and block-taking methods use the generic path.
struct MetalABI {Class cls;SEL selector;char signature[2048];};
static struct MetalABI *metal_abis;
static size_t metal_abi_count;
static Class metal_classes[8];
static int compare_metal_abi(const void *a,const void *b) {
    const struct MetalABI *x=a,*y=b;
    if(x->cls!=y->cls)return (uintptr_t)x->cls>(uintptr_t)y->cls?1:-1;
    return ((uintptr_t)x->selector>(uintptr_t)y->selector)-((uintptr_t)x->selector<(uintptr_t)y->selector);
}
static void prepare_metal_abis(void) {
    const char *names[]={"MTLRenderCommandEncoderInternal","MTLBlitCommandEncoderInternal",
        "MTLComputeCommandEncoderInternal","MTLCommandBufferInternal","MTLCommandQueueInternal",
        "MTLBufferInternal","MTLTextureInternal","MTLDeviceInternal"};
    size_t capacity=0;
    for(unsigned i=0;i<8;++i) {
        Class cls=metal_classes[i]=objc_getClass(names[i]);
        for(Class owner=cls;owner;owner=class_getSuperclass(owner)) {
            unsigned count=0;Method *methods=class_copyMethodList(owner,&count);
            for(unsigned j=0;j<count;++j) {
                SEL sel=method_getName(methods[j]);size_t k;
                for(k=0;k<metal_abi_count;++k)if(metal_abis[k].cls==cls && metal_abis[k].selector==sel)break;
                if(k<metal_abi_count)continue;
                char signature[2048];if(encode_signature(method_getTypeEncoding(methods[j]),signature,sizeof(signature)))continue;
                if(metal_abi_count==capacity) {
                    capacity=capacity?capacity*2:128;
                    void *p=realloc(metal_abis,capacity*sizeof(*metal_abis));if(!p)abort();metal_abis=p;
                }
                struct MetalABI *entry=&metal_abis[metal_abi_count++];entry->cls=cls;entry->selector=sel;
                strcpy(entry->signature,signature);
            }
            free(methods);
        }
    }
    qsort(metal_abis,metal_abi_count,sizeof(*metal_abis),compare_metal_abi);
}
static const char *metal_signature(Class cls,SEL selector) {
    static pthread_once_t once=PTHREAD_ONCE_INIT;pthread_once(&once,prepare_metal_abis);
    unsigned i;for(i=0;i<8;++i)if(metal_classes[i]==cls)break;
    if(i==8)return NULL;
    static __thread struct {Class cls;SEL selector;const char *signature;} cache[128];
    unsigned slot=(((uintptr_t)cls>>4)^((uintptr_t)selector>>3))&127;
    if(cache[slot].cls==cls && cache[slot].selector==selector)return cache[slot].signature;
    struct MetalABI key;key.cls=cls;key.selector=selector;
    struct MetalABI *entry=bsearch(&key,metal_abis,metal_abi_count,sizeof(key),compare_metal_abi);
    cache[slot].cls=cls;cache[slot].selector=selector;
    return cache[slot].signature=entry?entry->signature:NULL;
}
