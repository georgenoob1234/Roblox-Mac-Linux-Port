#pragma once
#include <math.h>

// Roblox can cache a drag position before CGAssociate requests capture, then
// repeatedly restore that stale position after capture. Recognize only points
// actually delivered during this press, rather than guessing by distance or
// blocking new engine positions such as the Shift Lock center.
typedef struct {
    int active;
    unsigned count,next;
    double x[64],y[64];
} RbxRightDragAnchor;

static inline void rbxRightDragTrack(RbxRightDragAnchor *state,double x,double y) {
    if(!state->active)return;
    state->x[state->next]=x;state->y[state->next]=y;
    state->next=(state->next+1)%64;
    if(state->count<64)++state->count;
}
static inline void rbxRightDragBegin(RbxRightDragAnchor *state,double x,double y,int active) {
    state->active=active;state->count=state->next=0;
    rbxRightDragTrack(state,x,y);
}
static inline int rbxRightDragRestoresMotion(const RbxRightDragAnchor *state,double x,double y) {
    if(!state->active)return 0;
    for(unsigned i=0;i<state->count;++i)
        if(fabs(state->x[i]-x)<1.0/256 && fabs(state->y[i]-y)<1.0/256)return 1;
    return 0;
}
