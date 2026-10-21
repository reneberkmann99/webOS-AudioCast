/* SPDX-License-Identifier: MIT
 * Test-only ALSA ABI double. NEVER installed on TV, NEVER real playback evidence.
 */
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
struct pcm { unsigned channels, bytes, reads; };
int snd_pcm_open(struct pcm **p,const char *name,int stream,int mode) {
  (void)name;(void)mode;
  if(stream!=1)return -EINVAL;
  if(getenv("FAKE_ALSA_UNAVAILABLE"))return -ENODEV;
  *p=calloc(1,sizeof(**p));(*p)->channels=2;(*p)->bytes=2;return 0;
}
int snd_pcm_close(struct pcm *p){free(p);return 0;}
int snd_pcm_hw_params_malloc(void **p){*p=malloc(1);return *p?0:-ENOMEM;}
void snd_pcm_hw_params_free(void *p){free(p);}
int snd_pcm_hw_params_any(struct pcm *p,void *h){(void)p;(void)h;return 0;}
int snd_pcm_hw_params_set_access(struct pcm *p,void *h,int a){(void)p;(void)h;return a==3?0:-EINVAL;}
int snd_pcm_format_value(const char *name){return !strcmp(name,"S16_LE")?2:!strcmp(name,"S32_LE")?10:-EINVAL;}
int snd_pcm_hw_params_set_format(struct pcm *p,void*h,int f){(void)h;p->bytes=f==2?2:4;return 0;}
int snd_pcm_hw_params_set_channels(struct pcm *p,void*h,unsigned c){(void)h;p->channels=c;return 0;}
int snd_pcm_hw_params_set_rate_near(struct pcm*p,void*h,unsigned*r,int*d){(void)p;(void)h;(void)d;if(getenv("FAKE_ALSA_WRONG_RATE"))*r=44100;return 0;}
int snd_pcm_hw_params_set_period_size_near(struct pcm*p,void*h,unsigned long*n,int*d){(void)p;(void)h;(void)n;(void)d;return 0;}
int snd_pcm_hw_params_set_buffer_size_near(struct pcm*p,void*h,unsigned long*n){(void)p;(void)h;(void)n;return 0;}
int snd_pcm_hw_params(struct pcm*p,void*h){(void)p;(void)h;return 0;}
int snd_pcm_prepare(struct pcm*p){(void)p;return 0;}
int snd_pcm_resume(struct pcm*p){(void)p;return getenv("FAKE_ALSA_SUSPEND")?-EAGAIN:0;}
int snd_pcm_wait(struct pcm*p,int ms){(void)p;(void)ms;struct timespec t={0,5000000};nanosleep(&t,0);return 1;}
long snd_pcm_readi(struct pcm*p,void *buf,unsigned long frames){
  if(getenv("FAKE_ALSA_SUSPEND"))return -ESTRPIPE;
  if(++p->reads%7==0)return -EPIPE;
  unsigned long n=frames/2;
  memset(buf,getenv("FAKE_ALSA_SILENCE")?0:0x12,n*p->channels*p->bytes);
  return (long)n;
}
int snd_pcm_recover(struct pcm*p,int err,int silent){(void)p;(void)silent;return err==-EPIPE?0:err;}
int snd_pcm_delay(struct pcm*p,long*d){(void)p;*d=24;return 0;}
const char *snd_strerror(int e){return e==-ENODEV?"test unavailable":"test ALSA error";}
int snd_pcm_hw_params_test_format(struct pcm*p,void*h,int f){(void)p;(void)h;return f==2||f==10?0:-EINVAL;}
int snd_pcm_hw_params_test_rate(struct pcm*p,void*h,unsigned r,int d){(void)p;(void)h;(void)d;return r==48000?0:-EINVAL;}
int snd_pcm_hw_params_test_channels(struct pcm*p,void*h,unsigned c){(void)p;(void)h;return c==2?0:-EINVAL;}
