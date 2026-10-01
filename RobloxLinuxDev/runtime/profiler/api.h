#pragma once
#include <stdint.h>
// Versioned plain-C boundary; units are milliseconds except item counters and
// RBX_PROF_UPLOAD_COPY_BYTES (bytes). Timing sample counts count operations.
enum { RBX_PROF_COMMIT, RBX_PROF_QUEUE, RBX_PROF_SCHEDULED, RBX_PROF_COMPLETION,
 RBX_PROF_WAIT, RBX_PROF_READBACK, RBX_PROF_PRESENT_ENQUEUE, RBX_PROF_PRESENT_DISPATCH,
 RBX_PROF_PRE_SUBMIT, RBX_PROF_SUBMIT, RBX_PROF_POST_SUBMIT, RBX_PROF_RESOURCES,
 RBX_PROF_UPLOADS, RBX_PROF_END_COMMAND, RBX_PROF_SYNC, RBX_PROF_WAITS,
 RBX_PROF_SIGNALS, RBX_PROF_READS, RBX_PROF_WRITES, RBX_PROF_GPU,
 RBX_PROF_SHADER, RBX_PROF_GRAPHICS_PIPELINE, RBX_PROF_COMPUTE_PIPELINE,
 RBX_PROF_UI_ROUNDTRIP, RBX_PROF_COMPOSITE, RBX_PROF_SWAP, RBX_PROF_HUD, RBX_PROF_DRAW, RBX_PROF_BINDINGS, RBX_PROF_PIPELINE_WAIT,
 RBX_PROF_UPLOAD_COPY, RBX_PROF_UPLOAD_COPY_BYTES, RBX_PROF_UPLOAD_ALLOC,
 RBX_PROF_UPLOAD_SUBMIT, RBX_PROF_UPLOAD_FENCE_WAIT, RBX_PROF_UPLOAD_QUEUE_LOCK,
 RBX_PROF_BLIT_TEXTURE, RBX_PROF_COMPUTE_DISPATCH, RBX_PROF_COMPUTE_BINDINGS,
 RBX_PROF_DESCRIPTOR_HITS, RBX_PROF_DESCRIPTOR_ALLOCS, RBX_PROF_DESCRIPTOR_REWRITES,
 RBX_PROF_ARENA_NEW, RBX_PROF_ARENA_OVERSIZE, RBX_PROF_ARENA_OVERFLOW,
 RBX_PROF_TEXTURE_DESTROY, RBX_PROF_BUFFER_DESTROY, RBX_PROF_COUNT };
// Geometry/resource diagnostics are deliberately separate from compatibility
// stages: they are bounded trace spans, not CPU/GPU accounting buckets.
enum {
 RBX_DIAG_BUFFER_CREATE, RBX_DIAG_BUFFER_VK_CREATE, RBX_DIAG_BUFFER_POOL_LOCK,
 RBX_DIAG_BUFFER_ALLOCATE, RBX_DIAG_BUFFER_MAP, RBX_DIAG_BUFFER_BIND,
 RBX_DIAG_BUFFER_COPY, RBX_DIAG_BUFFER_FLUSH, RBX_DIAG_BUFFER_BLIT,
 RBX_DIAG_COMMAND_CREATE, RBX_DIAG_COMMAND_COMMIT, RBX_DIAG_COMPLETION_REGISTER,
 RBX_DIAG_COMPLETION_LOCK, RBX_DIAG_COMPLETION_WAKE, RBX_DIAG_COMMAND_WAIT,
 // Version 7: upload synchronization and command-allocation attribution.
 RBX_DIAG_COMPLETION_SEMAPHORE, RBX_DIAG_SEMAPHORE_POOL_LOCK, RBX_DIAG_SEMAPHORE_CREATE,
 RBX_DIAG_TEXTURE_ACQUIRE, RBX_DIAG_COMMAND_SYNC, RBX_DIAG_QUEUE_SUBMIT, RBX_DIAG_QUEUE_LOCK,
 RBX_DIAG_COMMAND_POOL_LOCK, RBX_DIAG_COMMAND_POOL_CREATE, RBX_DIAG_COMMAND_BEGIN, RBX_DIAG_QUERY_SETUP,
 RBX_DIAG_COUNT
};
struct RbxProfilerSpan { uint64_t start_ns, cpu_ns, epoch; };
struct RbxProfilerAPI {
 uint32_t version;
 int (*enabled)(void);
 void (*metric)(unsigned,double);
 void (*frame)(void);
 void (*draw)(void);
 unsigned (*cpu_begin)(unsigned);
 void (*cpu_end)(unsigned);
 const void *(*allocator)(unsigned);
 unsigned (*gpu_begin)(unsigned);
 void (*gpu_end)(unsigned);
 void (*set_symbolizer)(int (*)(const void *,void *));
 // Version 5: whether the HUD is on screen, including while paused.
 int (*shown)(void);
 // Version 6: bounded geometry/resource trace spans.
 void (*span_begin)(struct RbxProfilerSpan *);
 void (*span_end)(const struct RbxProfilerSpan *,unsigned,uint64_t,uint64_t,uint64_t);
 // Version 8: the GTK host draws the HUD independently of game presentation.
 int (*hosted)(void);
};
