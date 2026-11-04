// An iOS UIKit app that waits for a tap on its button (mouse click on the Mac): prints the touch, the control event and a gesture, then exits. Run from an IPA by tools/ipa.py.
#import <UIKit/UIKit.h>
@interface VC : UIViewController
@end
@implementation VC
- (void)viewDidLoad {
    [super viewDidLoad]; self.view.backgroundColor = UIColor.systemBlueColor;
    UIButton *b = [UIButton buttonWithType:UIButtonTypeSystem]; b.frame = CGRectMake(40, 120, 240, 80); [b setTitle:@"Tap me" forState:UIControlStateNormal];
    b.backgroundColor = UIColor.whiteColor; b.titleLabel.font = [UIFont boldSystemFontOfSize:28];
    [b addTarget:self action:@selector(tapped:) forControlEvents:UIControlEventTouchUpInside]; [self.view addSubview:b];
}
- (void)tapped:(id)sender { printf("button tapped\n"); fflush(stdout); dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 2), dispatch_get_main_queue(), ^{ exit(0); }); }
@end
@interface AD : UIResponder <UIApplicationDelegate>
@property (strong, nonatomic) UIWindow *window;
@end
@implementation AD
- (BOOL)application:(UIApplication *)a didFinishLaunchingWithOptions:(NSDictionary *)o {
    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds]; self.window.rootViewController = [VC new]; [self.window makeKeyAndVisible];
    printf("ready\n"); fflush(stdout);
    if (!getenv("TAP_WAIT")) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{ exit(0); });
    return YES;
}
@end
int main(int argc, char **argv) { @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass([AD class])); } }
