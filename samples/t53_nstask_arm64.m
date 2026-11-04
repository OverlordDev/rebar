// NSTask starting an arm64-only executable: the x86_64 host kernel refuses to execute it, so the task has to run under rebar (Parallels' installer starts its
// arm64 app this way).  The parent starts the program given as argv[1] and prints what it wrote.
#import <Foundation/Foundation.h>
#include <stdio.h>
int main(int argc, char **argv) { @autoreleasepool {
    if (argc < 2) { printf("usage: t53 <program>\n"); return 2; }
    NSTask *t = [[NSTask alloc] init];
    t.launchPath = [NSString stringWithUTF8String:argv[1]];
    t.arguments = @[@"one", @"two"];
    NSPipe *p = [NSPipe pipe];
    t.standardOutput = p;
    [t launch];
    NSData *d = [[p fileHandleForReading] readDataToEndOfFile];
    [t waitUntilExit];
    NSString *s = [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding];
    // (drop rebar's own start-up chatter, if any)
    for (NSString *l in [s componentsSeparatedByString:@"\n"]) if (l.length && ![l hasPrefix:@"nvrm:"]) printf("child: %s\n", l.UTF8String);
    printf("status %d\n", t.terminationStatus);
} return 0; }
