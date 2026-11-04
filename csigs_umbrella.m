#import <Cocoa/Cocoa.h>
#import <CoreText/CoreText.h>
#import <ImageIO/ImageIO.h>
#import <QuartzCore/QuartzCore.h>
#include <ApplicationServices/ApplicationServices.h>
#if __has_include(<AVFoundation/AVFoundation.h>)
#import <AVFoundation/AVFoundation.h>
#endif
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>
#import <VideoToolbox/VideoToolbox.h>
#import <Accelerate/Accelerate.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <MetalPerformanceShaders/MetalPerformanceShaders.h>
#import <CoreImage/CoreImage.h>
#import <IOSurface/IOSurface.h>
#if __has_include(<Vision/Vision.h>)
#import <Vision/Vision.h>
#endif
#if __has_include(<CoreML/CoreML.h>)
#import <CoreML/CoreML.h>
#endif
#if __has_include(<SceneKit/SceneKit.h>)
#import <SceneKit/SceneKit.h>
#endif
#if __has_include(<CoreMIDI/CoreMIDI.h>)
#import <CoreMIDI/CoreMIDI.h>
#endif
#if __has_include(<ScreenCaptureKit/ScreenCaptureKit.h>)
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#endif
#include <os/lock.h>
#include <os/log.h>
#include <pthread.h>
#include <dispatch/dispatch.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <libkern/OSAtomic.h>
#include <sys/sysctl.h>
#include <objc/runtime.h>
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <IOKit/IOKitLib.h>
#include <SystemConfiguration/SystemConfiguration.h>
