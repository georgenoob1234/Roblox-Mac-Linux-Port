// Plain C ABI shared by Darling and the host decoder. No host pointers are freed in Darwin.
#ifndef TRACKA_VIDEO_HOST_H
#define TRACKA_VIDEO_HOST_H
#include <stdint.h>
#include <stddef.h>
typedef struct { int64_t value; int32_t scale; uint32_t flags; int64_t epoch; } VideoTime;
typedef struct { VideoTime pts, duration; void *refcon, *sample; uint32_t flags; } VideoStamp;
typedef struct {
    const uint8_t *data[3]; int stride[3], width, height, fullRange;
    VideoStamp stamp;
} VideoFrame;
void *tracka_video_open(uint32_t codec, const uint8_t *extra, size_t size, uint32_t pixelFormat);
void tracka_video_close(void *decoder);
int tracka_video_send(void *decoder, const uint8_t *data, size_t size, const VideoStamp *stamp);
int tracka_video_receive(void *decoder, VideoFrame *out);
int tracka_video_dimensions(void *decoder, int *width, int *height);
#endif
