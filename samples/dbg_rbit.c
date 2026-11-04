#include <arm_neon.h>
#include <stdint.h>
static long sys3(long n, long a, long b, long c) {
    register long x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c, x16 __asm__("x16") = n;
    __asm__ volatile("svc #0x80" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x16) : "memory", "cc");
    return x0;
}
static void hex(unsigned v){ char b[3]={"0123456789abcdef"[v>>4],"0123456789abcdef"[v&15],' '}; sys3(4,1,(long)b,3);}
static uint8_t rbit8(uint8_t x) { uint8_t r = 0; for (int i = 0; i < 8; i++) if (x & (1 << i)) r |= 0x80 >> i; return r; }
void start(void){
  uint8_t a[16], d[16]; for(int i=0;i<16;i++) a[i]=(uint8_t)(i*37+1);
  vst1q_u8(d, vrbitq_u8(vld1q_u8(a)));
  for(int i=0;i<16;i++){ hex(a[i]); hex(d[i]); hex(rbit8(a[i])); sys3(4,1,(long)"\n",1);}  
  sys3(1,0,0,0);
}
