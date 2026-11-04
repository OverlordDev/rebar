// Loading a compiled nib: outlets and actions wired to guest classes by name.
#import <Cocoa/Cocoa.h>
@interface NibOwner : NSObject
@property (strong) IBOutlet NSWindow *window;
@property (strong) IBOutlet NSTextField *label;
@property (assign) int presses;
- (IBAction)pressed:(id)sender;
@end
@implementation NibOwner
- (IBAction)pressed:(id)sender {
    self.presses++;
    self.label.stringValue = [NSString stringWithFormat:@"pressed %d times by %@", self.presses, NSStringFromClass([sender class])];
}
@end
int main(int argc, char **argv) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        NSString *path = [NSString stringWithUTF8String:argv[1]];
        NSData *data = [NSData dataWithContentsOfFile:path];
        printf("nib bytes: %lu\n", (unsigned long)data.length);
        NSNib *nib = [[NSNib alloc] initWithNibData:data bundle:nil];
        NibOwner *owner = [NibOwner new];
        NSArray *tops = nil;
        BOOL ok = [nib instantiateWithOwner:owner topLevelObjects:&tops];
        printf("instantiate: %d tops=%lu\n", ok, (unsigned long)tops.count);
        printf("window: %s frame=%.0fx%.0f title=%s\n", owner.window ? "yes" : "no", owner.window.frame.size.width, owner.window.frame.size.height, [owner.window.title UTF8String]);
        printf("label: %s\n", [owner.label.stringValue UTF8String]);
        NSButton *btn = nil;
        for (NSView *v in owner.window.contentView.subviews) if ([v isKindOfClass:[NSButton class]]) btn = (NSButton *)v;
        printf("button: %s target=%s\n", [btn.title UTF8String], [NSStringFromClass([btn.target class]) UTF8String]);
        [btn performClick:nil]; [btn performClick:nil];
        printf("after clicks: %s presses=%d\n", [owner.label.stringValue UTF8String], owner.presses);
    }
    return 0;
}
