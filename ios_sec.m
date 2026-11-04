// A keychain for iOS apps (loaded by rebar when REBAR_IOS is set): SecItemAdd / CopyMatching / Update / Delete on a plist in Application Support/Rebar/ios-keychains.
// The real Security framework gives a process that is a Mac Catalyst one only the data-protection keychain, which needs the keychain-access-groups entitlement: a restricted
// entitlement that an ad-hoc signed host bundle cannot carry (AMFI kills it), so every call would fail with -34018.
#import <Foundation/Foundation.h>
#import <Security/Security.h>
static NSMutableArray<NSMutableDictionary *> *g_items;
static NSString *g_path;
static NSLock *g_lock;
extern const char *getprogname(void);

static void load(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        g_lock = [NSLock new];
        NSString *dir = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/Rebar/ios-keychains"];
        [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
        g_path = [dir stringByAppendingPathComponent:[NSString stringWithFormat:@"%s.plist", getprogname()]];
        NSArray *a = [NSArray arrayWithContentsOfFile:g_path];
        g_items = [NSMutableArray array];
        for (NSDictionary *d in a) [g_items addObject:[d mutableCopy]];
    });
}
static void save(void) { [g_items writeToFile:g_path atomically:YES]; }

static BOOL is_meta(id k) {
    static NSSet *meta; static dispatch_once_t once;
    dispatch_once(&once, ^{ meta = [NSSet setWithArray:@[(__bridge id)kSecReturnData, (__bridge id)kSecReturnAttributes, (__bridge id)kSecReturnRef, (__bridge id)kSecReturnPersistentRef, (__bridge id)kSecMatchLimit,
        (__bridge id)kSecUseDataProtectionKeychain, (__bridge id)kSecAttrAccessGroup, (__bridge id)kSecAttrSynchronizable, (__bridge id)kSecAttrAccessible, (__bridge id)kSecValueData, (__bridge id)kSecUseOperationPrompt,
        (__bridge id)kSecUseAuthenticationUI, (__bridge id)kSecAttrAccessControl, (__bridge id)kSecUseKeychain, (__bridge id)kSecMatchSearchList, (__bridge id)kSecMatchCaseInsensitive, (__bridge id)kSecAttrCreationDate, (__bridge id)kSecAttrModificationDate]]; });
    return [meta containsObject:k];
}
static BOOL matches(NSDictionary *item, NSDictionary *q) {
    for (id k in q) { if (is_meta(k)) continue; id v = item[k]; if (!v || ![v isEqual:q[k]]) return NO; }
    return YES;
}
static NSArray *find(NSDictionary *q) { NSMutableArray *r = [NSMutableArray array]; for (NSMutableDictionary *i in g_items) if (matches(i, q)) [r addObject:i]; return r; }
static BOOL yes(NSDictionary *q, CFStringRef k) { return [q[(__bridge id)k] boolValue]; }

static id result_for(NSDictionary *item, NSDictionary *q) {
    BOOL d = yes(q, kSecReturnData), a = yes(q, kSecReturnAttributes);
    if (a) { NSMutableDictionary *r = [item mutableCopy]; if (!d) [r removeObjectForKey:(__bridge id)kSecValueData]; return r; }
    if (d) return item[(__bridge id)kSecValueData] ?: (id)[NSData data];
    return nil;
}

OSStatus rebar_SecItemAdd(CFDictionaryRef attrsRef, CFTypeRef *result) {
    load(); NSDictionary *attrs = (__bridge NSDictionary *)attrsRef; [g_lock lock];
    @try {
        if (!attrs[(__bridge id)kSecClass]) return errSecParam;
        NSMutableDictionary *key = [NSMutableDictionary dictionary];
        for (id k in attrs) if (!is_meta(k) && ![k isEqual:(__bridge id)kSecAttrLabel] && ![k isEqual:(__bridge id)kSecAttrComment] && ![k isEqual:(__bridge id)kSecAttrDescription]) key[k] = attrs[k];
        if (find(key).count) return errSecDuplicateItem;
        NSMutableDictionary *item = [NSMutableDictionary dictionary];
        for (id k in attrs) if (![k isEqual:(__bridge id)kSecReturnData] && ![k isEqual:(__bridge id)kSecReturnAttributes] && ![k isEqual:(__bridge id)kSecReturnRef] && ![k isEqual:(__bridge id)kSecReturnPersistentRef] && ![k isEqual:(__bridge id)kSecUseDataProtectionKeychain]) item[k] = attrs[k];
        item[(__bridge id)kSecAttrCreationDate] = item[(__bridge id)kSecAttrModificationDate] = [NSDate date];
        [g_items addObject:item]; save();
        if (result) { id r = result_for(item, attrs); *result = r ? CFBridgingRetain(r) : NULL; }
        return errSecSuccess;
    } @finally { [g_lock unlock]; }
}
OSStatus rebar_SecItemCopyMatching(CFDictionaryRef qRef, CFTypeRef *result) {
    load(); NSDictionary *q = (__bridge NSDictionary *)qRef; [g_lock lock];
    @try {
        NSArray *m = find(q); if (!m.count) return errSecItemNotFound;
        BOOL all = [q[(__bridge id)kSecMatchLimit] isEqual:(__bridge id)kSecMatchLimitAll];
        if (!result) return errSecSuccess;
        if (all) { NSMutableArray *r = [NSMutableArray array]; for (NSDictionary *i in m) { id x = result_for(i, q); if (x) [r addObject:x]; } *result = CFBridgingRetain(r); }
        else { id x = result_for(m.firstObject, q); *result = x ? CFBridgingRetain(x) : NULL; }
        return errSecSuccess;
    } @finally { [g_lock unlock]; }
}
OSStatus rebar_SecItemUpdate(CFDictionaryRef qRef, CFDictionaryRef upRef) {
    load(); NSDictionary *q = (__bridge NSDictionary *)qRef, *up = (__bridge NSDictionary *)upRef; [g_lock lock];
    @try {
        NSArray *m = find(q); if (!m.count) return errSecItemNotFound;
        for (NSMutableDictionary *i in m) { [i addEntriesFromDictionary:up]; i[(__bridge id)kSecAttrModificationDate] = [NSDate date]; }
        save(); return errSecSuccess;
    } @finally { [g_lock unlock]; }
}
OSStatus rebar_SecItemDelete(CFDictionaryRef qRef) {
    load(); NSDictionary *q = (__bridge NSDictionary *)qRef; [g_lock lock];
    @try {
        NSArray *m = find(q); if (!m.count) return errSecItemNotFound;
        [g_items removeObjectsInArray:m]; save(); return errSecSuccess;
    } @finally { [g_lock unlock]; }
}
