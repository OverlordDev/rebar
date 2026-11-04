// An iOS UIKit app (platform 2), run from an IPA by tools/ipa.py: UIApplicationMain, the app delegate gets didFinishLaunching, a window with a label is made key and visible, the app exits after a moment.
#import <UIKit/UIKit.h>
@interface AD : UIResponder <UIApplicationDelegate>
@property (strong, nonatomic) UIWindow *window;
@end
@implementation AD
- (BOOL)application:(UIApplication *)a didFinishLaunchingWithOptions:(NSDictionary *)o {
    UIWindow *w = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds]; UIViewController *vc = [UIViewController new];
    vc.view.backgroundColor = UIColor.systemOrangeColor;
    UILabel *l = [[UILabel alloc] initWithFrame:CGRectMake(20, 60, 300, 40)]; l.text = @"UIKit under rebar"; l.font = [UIFont boldSystemFontOfSize:24]; [vc.view addSubview:l];
    w.rootViewController = vc; [w makeKeyAndVisible]; self.window = w;
    printf("launched idiom %ld window key %d label %s\n", (long)UIDevice.currentDevice.userInterfaceIdiom, w.isKeyWindow, l.text.UTF8String); fflush(stdout);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{ printf("bye\n"); fflush(stdout); exit(0); });
    return YES;
}
@end
int main(int argc, char **argv) { @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass([AD class])); } }
