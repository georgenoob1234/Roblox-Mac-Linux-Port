#pragma once
#include <algorithm>

// Native input and EGL drawing run on different threads; the owner locks this
// state, never calls ImGui from the input thread, and only captures the grip.
struct ProfPanel {
 float x=0,y=0,width=0,height=0,view_width=1,view_height=1,grip=20;
 float start_x=0,start_y=0,start_width=0,start_height=0,start_left=0,start_top=0,scroll=0,title=22;
 bool dragging=false,moving=false,release=false,positioned=false;
 bool mouse(int kind,float nx,float ny,float wheel,bool available){
  float px=nx*view_width,py=ny*view_height;
  if(kind==4){dragging=moving=false;return false;} // blur, hide, or pointer capture
  if(kind==2){bool consumed=release;release=false;dragging=moving=false;return consumed;}
  if(moving && kind==0){x=start_left+px-start_x;y=start_top+py-start_y;positioned=true;return true;}
  if(dragging && kind==0){
   width=start_width+px-start_x;height=start_height+py-start_y;return true;
  }
  if(!available)return false;
  bool inside=width>0 && height>0 && px>=x && py>=y && px<=x+width && py<=y+height;
  if(kind==1 && inside && px>=x+width-grip && py>=y+height-grip){
   dragging=release=true;start_x=px;start_y=py;start_width=width;start_height=height;return true;
  }
  if(kind==1 && inside && py<=y+title){moving=release=true;start_x=px;start_y=py;start_left=x;start_top=y;return true;}
  if(kind==3 && inside){scroll+=wheel;return true;}
  return false;
 }
};
