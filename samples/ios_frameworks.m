// What iOS framework code does under rebar-ios: WKWebView, Metal, AVAudioSession, CoreMotion, CoreLocation, UIPasteboard, Keychain, CoreData in memory, NSURLSession to a local file URL, UIDocumentInteraction-free bits.
#import <UIKit/UIKit.h>
#import <WebKit/WebKit.h>
#import <Metal/Metal.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreMotion/CoreMotion.h>
#import <CoreLocation/CoreLocation.h>
#import <CoreData/CoreData.h>
#import <Security/Security.h>
#import <UserNotifications/UserNotifications.h>
@interface AD : UIResponder <UIApplicationDelegate, WKNavigationDelegate>
@property (strong, nonatomic) UIWindow *window; @property (strong) WKWebView *web;
@end
@implementation AD
- (BOOL)application:(UIApplication *)a didFinishLaunchingWithOptions:(NSDictionary *)o {
    self.window = [[UIWindow alloc] initWithFrame:CGRectMake(0, 0, 400, 800)]; UIViewController *vc = [UIViewController new]; self.window.rootViewController = vc; [self.window makeKeyAndVisible];
    self.web = [[WKWebView alloc] initWithFrame:CGRectMake(0, 0, 400, 400)]; self.web.navigationDelegate = self; [vc.view addSubview:self.web];
    [self.web loadHTMLString:@"<html><body><p id=a>hi</p></body></html>" baseURL:nil];
    return YES;
}
- (void)webView:(WKWebView *)w didFinishNavigation:(WKNavigation *)n {
    [w evaluateJavaScript:@"document.getElementById('a').textContent + (1+1)" completionHandler:^(id r, NSError *e) { printf("webkit js %s\n", [[r description] UTF8String]); fflush(stdout); [self rest]; }];
}
- (void)rest {
    id<MTLDevice> d = MTLCreateSystemDefaultDevice(); printf("metal device %s queue %d\n", d ? "yes" : "no", d && [d newCommandQueue] != nil); 
    NSError *err = nil; AVAudioSession *s = AVAudioSession.sharedInstance; BOOL ok = [s setCategory:AVAudioSessionCategoryPlayback error:&err]; printf("avaudiosession category ok %d\n", ok);
    printf("coremotion accelerometer %d\n", [CMMotionManager new].accelerometerAvailable);
    printf("corelocation services %d status %d\n", [CLLocationManager locationServicesEnabled], (int)[CLLocationManager new].authorizationStatus);
    UIPasteboard.generalPasteboard.string = @"pb-rebar"; printf("pasteboard %s\n", UIPasteboard.generalPasteboard.string.UTF8String);
    NSDictionary *q = @{(id)kSecClass: (id)kSecClassGenericPassword, (id)kSecAttrService: @"rebar.ios.test", (id)kSecAttrAccount: @"u"}; SecItemDelete((CFDictionaryRef)q);
    NSMutableDictionary *add = [q mutableCopy]; add[(id)kSecValueData] = [@"secret" dataUsingEncoding:NSUTF8StringEncoding]; OSStatus st = SecItemAdd((CFDictionaryRef)add, NULL);
    NSMutableDictionary *get = [q mutableCopy]; get[(id)kSecReturnData] = @YES; CFTypeRef out = NULL; OSStatus st2 = SecItemCopyMatching((CFDictionaryRef)get, &out);
    printf("keychain add %d get %d value %s\n", (int)st, (int)st2, out ? [[[NSString alloc] initWithData:(__bridge NSData *)out encoding:NSUTF8StringEncoding] UTF8String] : "-"); SecItemDelete((CFDictionaryRef)q);
    NSManagedObjectModel *m = [NSManagedObjectModel new]; NSEntityDescription *e = [NSEntityDescription new]; e.name = @"T"; NSAttributeDescription *at = [NSAttributeDescription new]; at.name = @"n"; at.attributeType = NSInteger32AttributeType; e.properties = @[at]; m.entities = @[e];
    NSPersistentStoreCoordinator *c = [[NSPersistentStoreCoordinator alloc] initWithManagedObjectModel:m]; [c addPersistentStoreWithType:NSInMemoryStoreType configuration:nil URL:nil options:nil error:nil];
    NSManagedObjectContext *ctx = [[NSManagedObjectContext alloc] initWithConcurrencyType:NSMainQueueConcurrencyType]; ctx.persistentStoreCoordinator = c;
    NSManagedObject *mo = [NSEntityDescription insertNewObjectForEntityForName:@"T" inManagedObjectContext:ctx]; [mo setValue:@42 forKey:@"n"]; [ctx save:nil];
    NSFetchRequest *fr = [NSFetchRequest fetchRequestWithEntityName:@"T"]; printf("coredata fetched %lu n=%s\n", (unsigned long)[ctx executeFetchRequest:fr error:nil].count, [[[[ctx executeFetchRequest:fr error:nil].firstObject valueForKey:@"n"] description] UTF8String]);
    NSString *p = [NSTemporaryDirectory() stringByAppendingPathComponent:@"ios_net.txt"]; [@"local file via URLSession" writeToFile:p atomically:YES encoding:NSUTF8StringEncoding error:nil];
    [[NSURLSession.sharedSession dataTaskWithURL:[NSURL fileURLWithPath:p] completionHandler:^(NSData *data, NSURLResponse *r, NSError *er) { printf("urlsession %s\n", data ? [[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] UTF8String] : er.description.UTF8String); fflush(stdout); exit(0); }] resume];
    fflush(stdout);
}
@end
int main(int argc, char **argv) { @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass([AD class])); } }
