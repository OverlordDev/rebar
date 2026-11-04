// Read-only constants of the system frameworks (structs of doubles live in __TEXT,__const: the importer must not take them for code).
// CATransform3DIdentity read as all zeros once, which made CoreAnimation flatten every layer of Chromium's GPU compositor to nothing.
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#include <stdio.h>
static void pr(const char *n, CATransform3D t) { printf("%-22s %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g\n", n, t.m11,t.m12,t.m13,t.m14,t.m21,t.m22,t.m23,t.m24,t.m31,t.m32,t.m33,t.m34,t.m41,t.m42,t.m43,t.m44); }
static void rc(const char *n, CGRect r) { printf("%-22s %g %g %g %g\n", n, r.origin.x, r.origin.y, r.size.width, r.size.height); }
int main() { @autoreleasepool {
    pr("CATransform3DIdentity", CATransform3DIdentity);
    printf("IsIdentity: %d\n", (int)CATransform3DIsIdentity(CATransform3DIdentity));
    CGAffineTransform a = CGAffineTransformIdentity;
    printf("CGAffineIdentity: %g %g %g %g %g %g\n", a.a, a.b, a.c, a.d, a.tx, a.ty);
    rc("CGRectZero", CGRectZero); rc("CGRectNull", CGRectNull); rc("CGRectInfinite", CGRectInfinite);
    printf("CGPointZero %g %g, CGSizeZero %g %g\n", CGPointZero.x, CGPointZero.y, CGSizeZero.width, CGSizeZero.height);
    CALayer *l = [CALayer layer]; l.sublayerTransform = CATransform3DIdentity;
    pr("layer.sublayerTransform", l.sublayerTransform);
} return 0; }
