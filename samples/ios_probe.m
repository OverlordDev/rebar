// Which frameworks an iOS app can load: dlopen of each (what an iOS binary links /System/Library/Frameworks/X.framework for).
#import <Foundation/Foundation.h>
#include <dlfcn.h>
int main(int argc, char **argv) { @autoreleasepool {
    const char *fw[] = {"UIKit","SwiftUI","Foundation","CoreData","CoreGraphics","CoreImage","CoreText","CoreAnimation","QuartzCore","AVFoundation","AVKit","AudioToolbox","CoreAudio","CoreMedia","CoreVideo","VideoToolbox","Metal","MetalKit","MetalPerformanceShaders",
        "SpriteKit","SceneKit","GameplayKit","GameController","GameKit","CoreMotion","CoreLocation","MapKit","WebKit","SafariServices","StoreKit","UserNotifications","CloudKit","Contacts","ContactsUI","EventKit","EventKitUI","Photos","PhotosUI","PencilKit",
        "Vision","VisionKit","CoreML","NaturalLanguage","Speech","SoundAnalysis","LocalAuthentication","Security","CryptoKit","Network","NetworkExtension","MultipeerConnectivity","Combine","Charts","WidgetKit","ActivityKit","AppIntents","Intents","SwiftData","TipKit",
        "ARKit","RealityKit","HealthKit","HomeKit","CallKit","PushKit","CoreBluetooth","CoreNFC","CoreHaptics","CoreTelephony","MessageUI","Messages","MediaPlayer","ReplayKit","AuthenticationServices","BackgroundTasks","FileProvider","QuickLook","QuickLookThumbnailing",
        "UniformTypeIdentifiers","StoreKitTest","MetricKit","OSLog","SystemConfiguration","Accelerate","ImageIO","PDFKit","Social","Accounts","iAd","AdSupport","AppTrackingTransparency","CoreSpotlight","CoreMIDI","CoreServices","MobileCoreServices","ExternalAccessory","DeviceCheck","AVFAudio","CoreLocationUI","MapKit","Translation","Observation","ShazamKit","MusicKit","CarPlay","DataDetection","LinkPresentation","NotificationCenter","ThreadNetwork","WatchConnectivity", NULL};
    int ok = 0, bad = 0; NSMutableArray *missing = [NSMutableArray array];
    for (int i = 0; fw[i]; i++) { char p[256]; snprintf(p, sizeof p, "/System/Library/Frameworks/%s.framework/%s", fw[i], fw[i]);
        if (dlopen(p, RTLD_NOW)) ok++; else { bad++; [missing addObject:@(fw[i])]; } }
    printf("loaded %d missing %d\nmissing: %s\n", ok, bad, [[missing componentsJoinedByString:@" "] UTF8String]);
} return 0; }
