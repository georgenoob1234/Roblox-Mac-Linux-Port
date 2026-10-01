#include "video-host.h"
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct { AVCodecContext *codec; AVFrame *frame, *pixels; struct SwsContext *sws; uint32_t pixelFormat; } Decoder;
void tracka_video_close(void *handle) {
    Decoder *d=handle;if(!d)return;
    sws_freeContext(d->sws);av_frame_free(&d->frame);av_frame_free(&d->pixels);
    avcodec_free_context(&d->codec);free(d);
}
void *tracka_video_open(uint32_t codec,const uint8_t *extra,size_t size,uint32_t pixelFormat) {
    enum AVCodecID id=codec==0x61766331?AV_CODEC_ID_H264:codec==0x68766331||codec==0x68657631?AV_CODEC_ID_HEVC:AV_CODEC_ID_NONE;
    const AVCodec *implementation=avcodec_find_decoder(id);
    if(!implementation||!extra||!size||size>INT_MAX-AV_INPUT_BUFFER_PADDING_SIZE)return NULL;
    Decoder *d=calloc(1,sizeof(*d));if(!d)return NULL;
    d->codec=avcodec_alloc_context3(implementation);d->frame=av_frame_alloc();d->pixels=av_frame_alloc();d->pixelFormat=pixelFormat;
    if(!d->codec||!d->frame||!d->pixels)goto fail;
    d->codec->extradata=av_mallocz(size+AV_INPUT_BUFFER_PADDING_SIZE);
    if(!d->codec->extradata)goto fail;
    memcpy(d->codec->extradata,extra,size);d->codec->extradata_size=(int)size;
    // ponytail: two software decoding threads per video; hardware decoding can follow profiling.
    d->codec->max_pixels=16384LL*16384;
    d->codec->thread_count=2;d->codec->flags|=AV_CODEC_FLAG_COPY_OPAQUE;
    if(avcodec_open2(d->codec,implementation,NULL)<0)goto fail;
    return d;
fail:tracka_video_close(d);return NULL;
}
// FFmpeg exposes dimensions only after a slice; CoreMedia needs them at SPS creation.
typedef struct { const uint8_t *data; size_t size,bit; int error; } Bits;
static unsigned bits(Bits *b,unsigned count) {
    if(count>32||b->bit>b->size*8||count>b->size*8-b->bit){b->error=1;return 0;}
    unsigned value=0;while(count--){value=value*2+((b->data[b->bit/8]>>(7-b->bit%8))&1);b->bit++;}return value;
}
static unsigned ue(Bits *b) {
    unsigned n=0;while(!b->error&&!bits(b,1)){if(++n>30){b->error=1;return 0;}}
    return b->error?0:((1u<<n)-1)+bits(b,n);
}
static int se(Bits *b){unsigned v=ue(b);return v&1?(int)(v/2+1):-(int)(v/2);}
static int spsDimensions(const uint8_t *data,size_t size,int hevc,int *width,int *height) {
    uint8_t *rbsp=malloc(size);if(!rbsp)return -1;
    size_t n=0;unsigned zeros=0;
    for(size_t i=hevc?2:1;i<size;i++) {
        if(zeros==2&&data[i]==3){zeros=0;continue;}
        rbsp[n++]=data[i];zeros=data[i]==0?zeros+1:0;
    }
    Bits b={rbsp,n,0,0};unsigned chroma=1,separate=0,frame=1,w=0,h=0,crop[4]={0};
    if(hevc) {
        bits(&b,4);unsigned layers=bits(&b,3);bits(&b,1);
        bits(&b,32);bits(&b,32);bits(&b,32);
        unsigned profile[8]={0},level[8]={0};
        for(unsigned i=0;i<layers;i++){profile[i]=bits(&b,1);level[i]=bits(&b,1);}
        if(layers)for(unsigned i=layers;i<8;i++)bits(&b,2);
        for(unsigned i=0;i<layers;i++){if(profile[i]){bits(&b,32);bits(&b,32);bits(&b,24);}if(level[i])bits(&b,8);}
        ue(&b);chroma=ue(&b);if(chroma==3)separate=bits(&b,1);w=ue(&b);h=ue(&b);
    } else {
        unsigned profile=bits(&b,8);bits(&b,16);ue(&b);
        if(profile==100||profile==110||profile==122||profile==244||profile==44||profile==83||profile==86||profile==118||profile==128||profile==138||profile==139||profile==134||profile==135) {
            chroma=ue(&b);if(chroma==3)separate=bits(&b,1);ue(&b);ue(&b);bits(&b,1);
            if(bits(&b,1))for(unsigned i=0;i<(chroma==3?12u:8u);i++)if(bits(&b,1)) {
                int last=8,next=8;for(unsigned j=0;j<(i<6?16u:64u);j++){if(next)next=(last+se(&b))&255;last=next?:last;}
            }
        }
        ue(&b);unsigned poc=ue(&b);
        if(poc==0)ue(&b);else if(poc==1){bits(&b,1);se(&b);se(&b);unsigned count=ue(&b);if(count>255)b.error=1;else for(unsigned i=0;i<count;i++)se(&b);}
        else if(poc>2)b.error=1;
        ue(&b);bits(&b,1);unsigned wm=ue(&b),hm=ue(&b);
        if(wm>1023||hm>1023)b.error=1;
        w=(wm+1)*16;h=(hm+1)*16;
        frame=bits(&b,1);if(!frame)bits(&b,1);h*=2-frame;bits(&b,1);
    }
    if(bits(&b,1))for(int i=0;i<4;i++)crop[i]=ue(&b);
    if(chroma>3)b.error=1;
    unsigned array=separate?0:chroma,cx=array==1||array==2?2:1,cy=(array==1?2:1)*(hevc?1:2-frame);
    uint64_t cropX=((uint64_t)crop[0]+crop[1])*cx,cropY=((uint64_t)crop[2]+crop[3])*cy;
    int result=-1;if(!b.error&&w>cropX&&h>cropY&&w<=16384&&h<=16384){*width=w-cropX;*height=h-cropY;result=0;}
    free(rbsp);return result;
}
int tracka_video_dimensions(void *handle,int *width,int *height) {
    Decoder *d=handle;if(!d||!width||!height)return -1;*width=*height=0;
    const uint8_t *data=d->codec->extradata;size_t size=d->codec->extradata_size;
    int hevc=d->codec->codec_id==AV_CODEC_ID_HEVC;
    for(size_t i=0;i+5<size;i++)if(!memcmp(data+i,"\0\0\0\1",4)) {
        size_t start=i+4,end=start;while(end+4<=size&&memcmp(data+end,"\0\0\0\1",4))end++;if(end+4>size)end=size;
        if((hevc?(data[start]>>1)&63:data[start]&31)==(hevc?33:7))return spsDimensions(data+start,end-start,hevc,width,height);
        i=end-1;
    }return -1;
}
int tracka_video_send(void *handle,const uint8_t *data,size_t size,const VideoStamp *stamp) {
    Decoder *d=handle;if(!d||size>INT_MAX)return -1;
    if(!data)return avcodec_send_packet(d->codec,NULL)<0?-1:0;
    if(!size||!stamp)return -1;
    AVPacket *packet=av_packet_alloc();if(!packet)return -1;
    if(av_new_packet(packet,(int)size)<0){av_packet_free(&packet);return -1;}
    memcpy(packet->data,data,size);
    packet->opaque_ref=av_buffer_alloc(sizeof(*stamp));
    if(!packet->opaque_ref){av_packet_free(&packet);return -1;}
    memcpy(packet->opaque_ref->data,stamp,sizeof(*stamp));
    int status=avcodec_send_packet(d->codec,packet);av_packet_free(&packet);
    return status==AVERROR(EAGAIN)?1:status<0?-1:0;
}
int tracka_video_receive(void *handle,VideoFrame *out) {
    Decoder *d=handle;if(!d||!out)return -1;
    av_frame_unref(d->frame);
    int status=avcodec_receive_frame(d->codec,d->frame);
    if(status==AVERROR(EAGAIN)||status==AVERROR_EOF)return 0;
    if(status<0)return -1;
    AVFrame *f=d->frame,*p=d->pixels;
    if(f->width<=0||f->height<=0||f->width>16384||f->height>16384)return -1;
    enum AVPixelFormat format=d->pixelFormat==0x42475241?AV_PIX_FMT_BGRA:d->pixelFormat==0x79343230||d->pixelFormat==0x66343230?AV_PIX_FMT_YUV420P:AV_PIX_FMT_NV12;
    if(!p->data[0]||p->width!=f->width||p->height!=f->height||p->format!=format) {
        av_frame_unref(p);p->width=f->width;p->height=f->height;p->format=format;
        if(av_frame_get_buffer(p,32)<0)return -1;
    }
    d->sws=sws_getCachedContext(d->sws,f->width,f->height,f->format,f->width,f->height,format,SWS_BILINEAR,NULL,NULL,NULL);
    if(!d->sws)return -1;
    const int *coeff=sws_getCoefficients(f->colorspace==AVCOL_SPC_BT709?SWS_CS_ITU709:SWS_CS_ITU601);
    int full=f->color_range==AVCOL_RANGE_JPEG;
    sws_setColorspaceDetails(d->sws,coeff,full,coeff,d->pixelFormat==0x42475241||d->pixelFormat==0x34323066||d->pixelFormat==0x66343230,0,1<<16,1<<16);
    if(sws_scale(d->sws,(const uint8_t *const *)f->data,f->linesize,0,f->height,p->data,p->linesize)!=f->height)return -1;
    memset(out,0,sizeof(*out));out->width=f->width;out->height=f->height;out->fullRange=full;
    for(int i=0;i<3;i++){out->data[i]=p->data[i];out->stride[i]=p->linesize[i];}
    if(f->opaque_ref&&f->opaque_ref->size==sizeof(out->stamp))memcpy(&out->stamp,f->opaque_ref->data,sizeof(out->stamp));
    return 1;
}
