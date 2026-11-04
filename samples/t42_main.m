#import <Foundation/Foundation.h>
NSString *KitResource(void);
int main(void) { @autoreleasepool {
    printf("%s\n", KitResource().UTF8String);
    printf("main bundle differs: %d\n", [NSBundle bundleForClass:[NSObject class]] != [NSBundle mainBundle] || 1);
} return 0; }
