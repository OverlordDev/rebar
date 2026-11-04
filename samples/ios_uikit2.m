// iOS UIKit behaviour from an IPA: Auto Layout, stack views, table view, navigation, image rendering, animations, defaults, text measurement.
#import <UIKit/UIKit.h>
@interface TVC : UITableViewController
@end
@implementation TVC
- (NSInteger)tableView:(UITableView *)t numberOfRowsInSection:(NSInteger)s { return 5; }
- (UITableViewCell *)tableView:(UITableView *)t cellForRowAtIndexPath:(NSIndexPath *)ip {
    UITableViewCell *c = [t dequeueReusableCellWithIdentifier:@"c"] ?: [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:@"c"];
    c.textLabel.text = [NSString stringWithFormat:@"row %ld", (long)ip.row]; return c;
}
@end
@interface AD : UIResponder <UIApplicationDelegate>
@property (strong, nonatomic) UIWindow *window;
@end
@implementation AD
- (BOOL)application:(UIApplication *)a didFinishLaunchingWithOptions:(NSDictionary *)o {
    self.window = [[UIWindow alloc] initWithFrame:CGRectMake(0, 0, 400, 800)];
    UIViewController *root = [UIViewController new]; root.title = @"Root"; root.view.backgroundColor = UIColor.whiteColor;
    UINavigationController *nav = [[UINavigationController alloc] initWithRootViewController:root];
    self.window.rootViewController = nav; [self.window makeKeyAndVisible];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC * 6 / 10), dispatch_get_main_queue(), ^{ [self run:root nav:nav]; });
    return YES;
}
- (void)run:(UIViewController *)root nav:(UINavigationController *)nav {
    printf("late: window %s root.view %s scene %d\n", NSStringFromCGRect(self.window.frame).UTF8String, NSStringFromCGRect(root.view.frame).UTF8String, self.window.windowScene != nil);
    // Auto Layout
    UIView *box = [UIView new]; box.translatesAutoresizingMaskIntoConstraints = NO; [root.view addSubview:box];
    [NSLayoutConstraint activateConstraints:@[[box.leadingAnchor constraintEqualToAnchor:root.view.leadingAnchor constant:20], [box.topAnchor constraintEqualToAnchor:root.view.topAnchor constant:100],
        [box.widthAnchor constraintEqualToConstant:200], [box.heightAnchor constraintEqualToAnchor:box.widthAnchor multiplier:0.5]]];
    [root.view layoutIfNeeded];
    printf("autolayout box %s\n", NSStringFromCGRect(box.frame).UTF8String);
    // stack view
    UIStackView *st = [[UIStackView alloc] initWithArrangedSubviews:@[[self v:50], [self v:70], [self v:30]]]; st.axis = UILayoutConstraintAxisVertical; st.spacing = 10; st.distribution = UIStackViewDistributionFill;
    st.translatesAutoresizingMaskIntoConstraints = NO; [root.view addSubview:st];
    [NSLayoutConstraint activateConstraints:@[[st.leadingAnchor constraintEqualToAnchor:root.view.leadingAnchor], [st.topAnchor constraintEqualToAnchor:root.view.topAnchor constant:300], [st.widthAnchor constraintEqualToConstant:100]]];
    [root.view layoutIfNeeded]; printf("stack %s\n", NSStringFromCGRect(st.frame).UTF8String);
    printf("app state %ld scene state %ld\n", (long)UIApplication.sharedApplication.applicationState, (long)self.window.windowScene.activationState);
    // table view
    TVC *tv = [[TVC alloc] initWithStyle:UITableViewStylePlain]; [nav pushViewController:tv animated:NO];
    [nav.view layoutIfNeeded]; [tv.view layoutIfNeeded]; printf("nav depth %lu top %s rows %ld\n", (unsigned long)nav.viewControllers.count, NSStringFromClass([nav.topViewController class]).UTF8String, (long)[tv.tableView numberOfRowsInSection:0]);
    UITableViewCell *cell = [tv.tableView cellForRowAtIndexPath:[NSIndexPath indexPathForRow:2 inSection:0]]; printf("cell2 text %s table frame %s visible %lu\n", cell.textLabel.text.UTF8String, NSStringFromCGRect(tv.tableView.frame).UTF8String, (unsigned long)tv.tableView.visibleCells.count);
    // image rendering
    UIGraphicsImageRenderer *r = [[UIGraphicsImageRenderer alloc] initWithSize:CGSizeMake(4, 4)];
    UIImage *img = [r imageWithActions:^(UIGraphicsImageRendererContext *ctx) { [UIColor.redColor setFill]; [ctx fillRect:CGRectMake(0, 0, 4, 4)]; }];
    CGImageRef cg = img.CGImage; CFDataRef d = CGDataProviderCopyData(CGImageGetDataProvider(cg)); const uint8_t *px = CFDataGetBytePtr(d);
    printf("image %gx%g scale %g pixel %02x%02x%02x%02x\n", img.size.width, img.size.height, img.scale, px[0], px[1], px[2], px[3]); CFRelease(d);
    // text measurement and fonts
    UIFont *f = [UIFont systemFontOfSize:17]; CGSize sz = [@"Hello" sizeWithAttributes:@{NSFontAttributeName: f}]; printf("text width ok %d height ok %d\n", sz.width > 30 && sz.width < 60, sz.height > 15 && sz.height < 25);
    // defaults, pasteboard
    [[NSUserDefaults standardUserDefaults] setObject:@"v" forKey:@"rebar.k"]; printf("defaults %s\n", [[NSUserDefaults standardUserDefaults] stringForKey:@"rebar.k"].UTF8String);
    // trait collection, device
    printf("idiom %ld\n", (long)UIDevice.currentDevice.userInterfaceIdiom);
    // alert controller
    UIAlertController *al = [UIAlertController alertControllerWithTitle:@"T" message:@"M" preferredStyle:UIAlertControllerStyleAlert]; [al addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
    printf("alert actions %lu\n", (unsigned long)al.actions.count);
    // animation completion
    UIView *anim = [[UIView alloc] initWithFrame:CGRectMake(0, 0, 10, 10)]; [tv.view addSubview:anim];
    [UIView animateWithDuration:0.2 animations:^{ anim.alpha = 0.0; } completion:^(BOOL fin) { printf("animation finished %d alpha %g\n", fin, anim.alpha); fflush(stdout); exit(0); }];
    fflush(stdout);
}
- (UIView *)v:(CGFloat)h { UIView *v = [UIView new]; [v.heightAnchor constraintEqualToConstant:h].active = YES; return v; }
@end
int main(int argc, char **argv) { @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass([AD class])); } }
