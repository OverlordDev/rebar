// NSBundle's path cache (a CFDictionary that grows and rehashes) with the default malloc: the process must not abort in libmalloc's nano zone.
#import <Foundation/Foundation.h>
int main() {
    @autoreleasepool {
        int n = 0;
        for (int i = 0; i < 300; i++) {
            NSString *p = [NSString stringWithFormat:@"/Applications/Safari.app/Contents/Frameworks/../%d", i % 40];
            NSBundle *b = [NSBundle bundleWithPath:p];
            (void)b; n++;
        }
        NSLog(@"bundles %d", n);
        printf("bundles %d\n", n);
    }
    return 0;
}
