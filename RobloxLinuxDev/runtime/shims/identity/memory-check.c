// Run in the Darwin guest: validates direct Linux reads and both Mach ABIs.
#include <assert.h>
#include <stdio.h>
#include <mach/mach_time.h>
#include "memory.c"
int main(void) {
    vm_statistics_data_t small;vm_statistics64_data_t large;
    mach_msg_type_number_t count=HOST_VM_INFO_COUNT;
    assert(host_statistics(0,HOST_VM_INFO,(host_info_t)&small,&count)==KERN_INVALID_ARGUMENT);
    count=0;
    assert(host_statistics(1,HOST_VM_INFO,(host_info_t)&small,&count)==KERN_FAILURE);
    const unsigned revisions[]={HOST_VM_INFO_REV0_COUNT,HOST_VM_INFO_REV1_COUNT,HOST_VM_INFO_COUNT};
    for(unsigned i=0;i<3;++i) {
        memset(&small,0xa5,sizeof(small));count=revisions[i];
        assert(!host_statistics(1,HOST_VM_INFO,(host_info_t)&small,&count));
        assert(count==revisions[i] && small.free_count && small.inactive_count);
        for(unsigned j=count*sizeof(integer_t);j<sizeof(small);++j)assert(((unsigned char*)&small)[j]==0xa5);
    }
    for(unsigned i=0;i<2;++i) {
        memset(&large,0xa5,sizeof(large));count=i?HOST_VM_INFO64_COUNT:HOST_VM_INFO64_REV0_COUNT;
        unsigned expected=count;
        assert(!host_statistics64(1,HOST_VM_INFO64,(host_info64_t)&large,&count));
        assert(count==expected && large.free_count && large.inactive_count);
        for(unsigned j=count*sizeof(integer_t);j<sizeof(large);++j)assert(((unsigned char*)&large)[j]==0xa5);
    }
    unsigned long long total=(unsigned long long)large.free_count+large.active_count+large.inactive_count+large.wire_count;
    assert(total>262144); // At least 1 GiB; catches an empty/stub result.
    uint64_t max=0,start=mach_absolute_time();
    for(unsigned i=0;i<10000;++i) {
        uint64_t begin=mach_absolute_time();count=HOST_VM_INFO_COUNT;
        assert(!host_statistics(1,HOST_VM_INFO,(host_info_t)&small,&count));
        uint64_t elapsed=mach_absolute_time()-begin;if(elapsed>max)max=elapsed;
    }
    mach_timebase_info_data_t scale;mach_timebase_info(&scale);
    printf("PASS live host memory, short-buffer guards and both ABIs; 10000 queries %.3f ms, max %.3f ms\n",
        (mach_absolute_time()-start)*(double)scale.numer/scale.denom/1e6,max*(double)scale.numer/scale.denom/1e6);
}
