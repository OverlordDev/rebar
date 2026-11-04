// A class defined in a framework of the app: its bundle (bundleForClass:) is the framework, not the main bundle, and its resources are found there.
#import <Foundation/Foundation.h>
@interface KitThing : NSObject @end
@implementation KitThing @end
NSString *KitResource(void) {
    NSBundle *b = [NSBundle bundleForClass:[KitThing class]];
    NSString *p = [b pathForResource:@"hello" ofType:@"txt"];
    return [NSString stringWithFormat:@"bundle=%@ resource=%@", b.bundlePath.lastPathComponent, p ? [[NSString stringWithContentsOfFile:p encoding:NSUTF8StringEncoding error:nil] stringByTrimmingCharactersInSet:NSCharacterSet.newlineCharacterSet] : @"(none)"];
}
