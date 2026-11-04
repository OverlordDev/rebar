// An Objective-C method whose NSRange argument comes after seven integer arguments: AAPCS64 puts the whole struct on the stack (x7 stays unused),
// SysV needs the registers and the stack in another order. GLFW's key handling (-[NSString getBytes:maxLength:usedLength:encoding:options:range:remainingRange:])
#import <Foundation/Foundation.h>
int main(void) { @autoreleasepool {
    NSString *s = @"k";
    uint32_t cp = 0; NSUInteger used = 0; NSRange rem;
    BOOL ok = [s getBytes:&cp maxLength:4 usedLength:&used encoding:NSUTF32LittleEndianStringEncoding options:0 range:NSMakeRange(0, 1) remainingRange:&rem];
    printf("ok %d codepoint %u used %lu remaining %lu+%lu\n", ok, cp, (unsigned long)used, (unsigned long)rem.location, (unsigned long)rem.length);
} return 0; }
