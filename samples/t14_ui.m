// More AppKit: table view data source, menus, KVO, Auto Layout, timers, undo, NSCoding.
#import <Cocoa/Cocoa.h>
#define OUT(fmt, ...) printf("%s\n", [[NSString stringWithFormat:fmt, ##__VA_ARGS__] UTF8String])

@interface Person : NSObject <NSSecureCoding>
@property (copy) NSString *name;
@property (assign) NSInteger age;
@end
@implementation Person
+ (BOOL)supportsSecureCoding { return YES; }
- (instancetype)initWithCoder:(NSCoder *)c { if ((self = [super init])) { _name = [c decodeObjectOfClass:[NSString class] forKey:@"n"]; _age = [c decodeIntegerForKey:@"a"]; } return self; }
- (void)encodeWithCoder:(NSCoder *)c { [c encodeObject:_name forKey:@"n"]; [c encodeInteger:_age forKey:@"a"]; }
@end

@interface Tile : NSView
@property (assign) CGFloat hue;
@end
@implementation Tile
+ (BOOL)supportsSecureCoding { return YES; }
- (instancetype)initWithCoder:(NSCoder *)c { if ((self = [super initWithCoder:c])) _hue = [c decodeDoubleForKey:@"hue"]; return self; }
- (void)encodeWithCoder:(NSCoder *)c { [super encodeWithCoder:c]; [c encodeDouble:_hue forKey:@"hue"]; }
- (NSSize)intrinsicContentSize { return NSMakeSize(60 + self.hue * 10, 30); }
@end

@interface Model : NSObject <NSTableViewDataSource, NSTableViewDelegate>
@property (strong) NSMutableArray<Person *> *people;
@property (assign) NSInteger counter;
@property (strong) NSMutableArray *kvoLog;
@property (assign) int menuHits;
@end
@implementation Model
- (instancetype)init { if ((self = [super init])) { _kvoLog = [NSMutableArray array]; _people = [NSMutableArray array]; } return self; }
- (NSInteger)numberOfRowsInTableView:(NSTableView *)tv { return (NSInteger)self.people.count; }
- (id)tableView:(NSTableView *)tv objectValueForTableColumn:(NSTableColumn *)col row:(NSInteger)row {
    Person *p = self.people[row];
    return [col.identifier isEqualToString:@"name"] ? p.name : @(p.age);
}
- (void)observeValueForKeyPath:(NSString *)kp ofObject:(id)o change:(NSDictionary *)ch context:(void *)ctx {
    [self.kvoLog addObject:[NSString stringWithFormat:@"%@: %@ -> %@", kp, ch[NSKeyValueChangeOldKey], ch[NSKeyValueChangeNewKey]]];
}
- (void)menuHit:(NSMenuItem *)item { self.menuHits++; OUT(@"menu hit: %@ (tag %ld)", item.title, (long)item.tag); }
@end

static Model *g_model;
int main(void) {
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        g_model = [Model new];
        for (int i = 0; i < 5; i++) { Person *p = [Person new]; p.name = [NSString stringWithFormat:@"Person %d", i]; p.age = 20 + i * 3; [g_model.people addObject:p]; }

        // table view backed by guest data source
        NSScrollView *sv = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 300, 150)];
        NSTableView *tv = [[NSTableView alloc] initWithFrame:sv.bounds];
        for (NSString *ident in @[@"name", @"age"]) {
            NSTableColumn *col = [[NSTableColumn alloc] initWithIdentifier:ident];
            col.title = ident; col.width = 120; [tv addTableColumn:col];
        }
        tv.dataSource = g_model; tv.delegate = g_model;
        sv.documentView = tv;
        [tv reloadData];
        OUT(@"rows=%ld cols=%lu", (long)tv.numberOfRows, (unsigned long)tv.numberOfColumns);
        NSRect r1 = [tv rectOfRow:2];
        OUT(@"rectOfRow(2): %.0f %.0f %.0f %.0f", r1.origin.x, r1.origin.y, r1.size.width, r1.size.height);
        OUT(@"rowAtPoint: %ld", (long)[tv rowAtPoint:NSMakePoint(10, r1.origin.y + 3)]);
        id v = [tv.dataSource tableView:tv objectValueForTableColumn:tv.tableColumns[1] row:3];
        OUT(@"cell(3,age)=%@ cell(1,name)=%@", v, [tv.dataSource tableView:tv objectValueForTableColumn:tv.tableColumns[0] row:1]);

        // KVO on a guest class
        [g_model addObserver:g_model forKeyPath:@"counter" options:NSKeyValueObservingOptionOld | NSKeyValueObservingOptionNew context:NULL];
        g_model.counter = 5; g_model.counter = 9;
        [g_model removeObserver:g_model forKeyPath:@"counter"];
        OUT(@"kvo: %@", [g_model.kvoLog componentsJoinedByString:@" | "]);

        // menus
        NSMenu *menu = [[NSMenu alloc] initWithTitle:@"Test"];
        for (int i = 0; i < 3; i++) {
            NSMenuItem *it = [menu addItemWithTitle:[NSString stringWithFormat:@"Item %d", i] action:@selector(menuHit:) keyEquivalent:i == 1 ? @"k" : @""];
            it.target = g_model; it.tag = 100 + i;
        }
        [menu performActionForItemAtIndex:2]; [menu performActionForItemAtIndex:0];
        OUT(@"menu: %lu items, hits=%d, key=%@", (unsigned long)menu.numberOfItems, g_model.menuHits, [menu itemAtIndex:1].keyEquivalent);

        // Auto Layout
        NSView *host = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 400, 200)];
        Tile *a = [[Tile alloc] initWithFrame:NSZeroRect]; a.hue = 2; a.translatesAutoresizingMaskIntoConstraints = NO;
        Tile *b = [[Tile alloc] initWithFrame:NSZeroRect]; b.hue = 4; b.translatesAutoresizingMaskIntoConstraints = NO;
        [host addSubview:a]; [host addSubview:b];
        [NSLayoutConstraint activateConstraints:@[
            [a.leadingAnchor constraintEqualToAnchor:host.leadingAnchor constant:12],
            [a.topAnchor constraintEqualToAnchor:host.topAnchor constant:8],
            [b.leadingAnchor constraintEqualToAnchor:a.trailingAnchor constant:20],
            [b.centerYAnchor constraintEqualToAnchor:a.centerYAnchor],
            [b.widthAnchor constraintEqualToConstant:90]]];
        [host layoutSubtreeIfNeeded];
        NSRect fa = a.frame, fb = b.frame;
        OUT(@"layout a=%.0f,%.0f %.0fx%.0f b=%.0f,%.0f %.0fx%.0f", fa.origin.x, fa.origin.y, fa.size.width, fa.size.height, fb.origin.x, fb.origin.y, fb.size.width, fb.size.height);

        // NSCoding round trip through NSKeyedArchiver (what nib loading does)
        NSError *err = nil;
        NSData *data = [NSKeyedArchiver archivedDataWithRootObject:@[g_model.people[1], a] requiringSecureCoding:NO error:&err];
        NSKeyedUnarchiver *ua = [[NSKeyedUnarchiver alloc] initForReadingFromData:data error:&err];
        ua.requiresSecureCoding = NO;
        NSArray *back = [ua decodeObjectForKey:NSKeyedArchiveRootObjectKey];
        Person *p2 = back[0]; Tile *t2 = back[1];
        OUT(@"coding: %@/%ld %@ hue=%.1f frameW=%.0f err=%@", p2.name, (long)p2.age, NSStringFromClass([t2 class]), t2.hue, t2.frame.size.width, err);

        // timer + notification + undo
        __block int ticks = 0;
        NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
        id token = [nc addObserverForName:@"RebarPing" object:nil queue:nil usingBlock:^(NSNotification *n) { OUT(@"ping %@", n.userInfo[@"k"]); }];
        NSTimer *timer = [NSTimer scheduledTimerWithTimeInterval:0.05 repeats:YES block:^(NSTimer *t) {
            ticks++;
            [nc postNotificationName:@"RebarPing" object:nil userInfo:@{@"k": @(ticks)}];
            if (ticks == 3) { [t invalidate]; [nc removeObserver:token]; CFRunLoopStop(CFRunLoopGetMain()); }
        }];
        (void)timer;
        CFRunLoopRun();
        NSUndoManager *um = [NSUndoManager new];
        __block int val = 1;
        [um registerUndoWithTarget:g_model handler:^(id target) { val = 7; }];
        [um undo];
        OUT(@"timer ticks=%d undo val=%d", ticks, val);

        // images and colors
        NSImage *img = [[NSImage alloc] initWithSize:NSMakeSize(16, 16)];
        [img lockFocus]; [[NSColor redColor] setFill]; NSRectFill(NSMakeRect(0, 0, 16, 16)); [img unlockFocus];
        NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithData:[img TIFFRepresentation]];
        NSColor *px = [rep colorAtX:4 y:4];
        OUT(@"image %.0fx%.0f px r=%.2f g=%.2f", img.size.width, img.size.height, px.redComponent, px.greenComponent);
    }
    return 0;
}
