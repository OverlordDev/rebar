// An iOS app with a UITextField: waits for the key events of the Mac keyboard (typing "hi" and Return) and prints the text and the delegate callbacks. Run by hand with tools/ipa.py (needs a person or a synthetic keyboard).
#import <UIKit/UIKit.h>
@interface AD : UIResponder <UIApplicationDelegate, UITextFieldDelegate>
@property (strong, nonatomic) UIWindow *window; @property (strong) UITextField *tf;
@end
@implementation AD
- (BOOL)application:(UIApplication *)a didFinishLaunchingWithOptions:(NSDictionary *)o {
    self.window = [[UIWindow alloc] initWithFrame:CGRectMake(0, 0, 400, 400)]; UIViewController *vc = [UIViewController new]; vc.view.backgroundColor = UIColor.whiteColor; self.window.rootViewController = vc;
    self.tf = [[UITextField alloc] initWithFrame:CGRectMake(20, 100, 300, 40)]; self.tf.borderStyle = UITextBorderStyleRoundedRect; self.tf.delegate = self; [vc.view addSubview:self.tf];
    [self.window makeKeyAndVisible]; [self.tf becomeFirstResponder]; printf("ready first responder %d\n", self.tf.isFirstResponder); fflush(stdout); return YES;
}
- (BOOL)textField:(UITextField *)t shouldChangeCharactersInRange:(NSRange)r replacementString:(NSString *)s { printf("change '%s'\n", s.UTF8String); fflush(stdout); return YES; }
- (BOOL)textFieldShouldReturn:(UITextField *)t { printf("return text '%s'\n", t.text.UTF8String); fflush(stdout); exit(0); return YES; }
@end
int main(int argc, char **argv) { @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass([AD class])); } }
