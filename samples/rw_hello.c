#include <stdio.h>
#include <stdlib.h>
int main(int argc,char**argv){ const char *o=getenv("RW_OUT"); FILE *f=o?fopen(o,"w"):stdout; fprintf(f,"hello from arm64 app, argc=%d argv1=%s\n",argc,argc>1?argv[1]:"-"); if(o)fclose(f); return 0; }
