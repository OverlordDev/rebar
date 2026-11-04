#import <Foundation/Foundation.h>
int main(int argc, char **argv) {
    printf("argv0=%s\n", argv[0]);
    printf("procname=%s\n", [[[NSProcessInfo processInfo] processName] UTF8String]);
    printf("args=%s\n", [[[[NSProcessInfo processInfo] arguments] description] UTF8String]);
    printf("bundle=%s\n", [[[NSBundle mainBundle] executablePath] UTF8String]);
    return 0;
}
