#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include "host/plugin_api_v1.h"
static uint8_t mailbox[4096];
int main(){
  void* lib=dlopen("build/chompi_move.so",RTLD_NOW);
  if(!lib){printf("%s\n",dlerror());return 1;}
  auto init=(plugin_api_v2_t*(*)(const host_api_v1_t*))dlsym(lib,"move_plugin_init_v2");
  static host_api_v1_t h; memset(&h,0,sizeof(h));
  h.api_version=MOVE_PLUGIN_API_VERSION; h.sample_rate=44100; h.frames_per_block=128;
  h.mapped_memory=mailbox; h.audio_out_offset=MOVE_AUDIO_OUT_OFFSET; h.audio_in_offset=MOVE_AUDIO_IN_OFFSET;
  auto api=init(&h); void* inst=api->create_instance(".",NULL);
  api->set_param(inst,"sample_dir","samples");
  api->set_param(inst,"voice_mode","cubbi");
  char buf[256];
  for(int i=0;i<6;i++){
    usleep(200*1000);
    int n=api->get_param(inst,"debug_files",buf,sizeof(buf));
    printf("t=%dms n=%d %s\n", i*200, n, n>0?buf:"(no answer)");
  }
  // now trigger slot 1 and render 1 s
  uint8_t on[3]={0x90,48,100}; api->on_midi(inst,on,3,MOVE_MIDI_SOURCE_INTERNAL);
  int16_t out[256]; long peak=0;
  for(int b=0;b<344;b++){ api->render_block(inst,out,128); for(int i=0;i<256;i++){int v=out[i]<0?-out[i]:out[i]; if(v>peak)peak=v;} usleep(2900);}
  printf("after note: peak=%ld\n", peak);
  api->get_param(inst,"debug_files",buf,sizeof(buf)); printf("%s\n",buf);
  char err[160]; if(api->get_error(inst,err,sizeof(err))>0) printf("err: %s\n",err);
  return 0;
}
