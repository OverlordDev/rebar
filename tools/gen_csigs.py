#!/usr/bin/env python3
"""Generate csigs.h: type encodings of exported C functions whose signature has a by-value
struct that arm64 and x86_64 pass differently (so the generic register forwarder would be wrong).

Uses libclang straight from Xcode through ctypes (no pip packages)."""
import ctypes, os, subprocess, sys

LIB = "/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/lib/libclang.dylib"
cl = ctypes.CDLL(LIB)

class CXString(ctypes.Structure): _fields_ = [("data", ctypes.c_void_p), ("flags", ctypes.c_uint)]
class CXCursor(ctypes.Structure): _fields_ = [("kind", ctypes.c_int), ("xdata", ctypes.c_int), ("data", ctypes.c_void_p * 3)]
class CXType(ctypes.Structure): _fields_ = [("kind", ctypes.c_int), ("data", ctypes.c_void_p * 2)]

def sig(name, res, *args):
    f = getattr(cl, name); f.restype = res; f.argtypes = list(args); return f

createIndex = sig("clang_createIndex", ctypes.c_void_p, ctypes.c_int, ctypes.c_int)
parse = sig("clang_parseTranslationUnit", ctypes.c_void_p, ctypes.c_void_p, ctypes.c_char_p,
            ctypes.POINTER(ctypes.c_char_p), ctypes.c_int, ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint)
tuCursor = sig("clang_getTranslationUnitCursor", CXCursor, ctypes.c_void_p)
VISITOR = ctypes.CFUNCTYPE(ctypes.c_int, CXCursor, CXCursor, ctypes.c_void_p)
visit = sig("clang_visitChildren", ctypes.c_uint, CXCursor, VISITOR, ctypes.c_void_p)
curKind = sig("clang_getCursorKind", ctypes.c_int, CXCursor)
spelling = sig("clang_getCursorSpelling", CXString, CXCursor)
cstr = sig("clang_getCString", ctypes.c_char_p, CXString)
dispose = sig("clang_disposeString", None, CXString)
curType = sig("clang_getCursorType", CXType, CXCursor)
resType = sig("clang_getResultType", CXType, CXType)
numArgs = sig("clang_Cursor_getNumArguments", ctypes.c_int, CXCursor)
getArg = sig("clang_Cursor_getArgument", CXCursor, CXCursor, ctypes.c_uint)
canon = sig("clang_getCanonicalType", CXType, CXType)
sizeOf = sig("clang_Type_getSizeOf", ctypes.c_longlong, CXType)
alignOf = sig("clang_Type_getAlignOf", ctypes.c_longlong, CXType)
typeDecl = sig("clang_getTypeDeclaration", CXCursor, CXType)
FIELDV = ctypes.CFUNCTYPE(ctypes.c_int, CXCursor, ctypes.c_void_p)
visitFields = sig("clang_Type_visitFields", ctypes.c_uint, CXType, FIELDV, ctypes.c_void_p)
elemType = sig("clang_getArrayElementType", CXType, CXType)
arrSize = sig("clang_getArraySize", ctypes.c_longlong, CXType)
enumInt = sig("clang_getEnumDeclIntegerType", CXType, CXCursor)
variadic = sig("clang_isFunctionTypeVariadic", ctypes.c_uint, CXType)
storage = sig("clang_Cursor_getStorageClass", ctypes.c_int, CXCursor)
cursorKindOfDecl = sig("clang_getCursorKind", ctypes.c_int, CXCursor)

# CXTypeKind
K = dict(Void=2, Bool=3, Char_U=4, UChar=5, UShort=8, UInt=9, ULong=10, ULongLong=11, Char_S=13, SChar=14,
         Short=16, Int=17, Long=18, LongLong=19, Float=21, Double=22, LongDouble=23, Pointer=101, Block=102,
         Record=105, Enum=106, ObjCPtr=109, ConstArr=112, Vector=113, Elab=119, Typedef=107, Complex=100)
FUNCTION_DECL = 8
STATIC = 3

class Unsupported(Exception): pass

def enc(t):
    """(encoding, size, align, hfa_count, hfa_char) of a canonical type."""
    t = canon(t)
    k = t.kind
    ints = {K['Bool']: ('B', 1), K['Char_U']: ('C', 1), K['UChar']: ('C', 1), K['Char_S']: ('c', 1), K['SChar']: ('c', 1),
            K['UShort']: ('S', 2), K['Short']: ('s', 2), K['UInt']: ('I', 4), K['Int']: ('i', 4),
            K['ULong']: ('Q', 8), K['ULongLong']: ('Q', 8), K['Long']: ('q', 8), K['LongLong']: ('q', 8)}
    if k == K['Void']: return ('v', 0, 1, 0, '')
    if k in ints: e, n = ints[k]; return (e, n, n, 0, '')
    if k == K['Float']: return ('f', 4, 4, 1, 'f')
    if k == K['Double']: return ('d', 8, 8, 1, 'd')
    if k in (K['Pointer'], K['ObjCPtr']): return ('^v' if k == K['Pointer'] else '@', 8, 8, 0, '')
    if k == K['Block']: return ('@?', 8, 8, 0, '')
    if k == K['Enum']: return enc(enumInt(typeDecl(t)))
    if k == K['ConstArr']:
        e, sz, al, h, hc = enc(elemType(t)); n = arrSize(t)
        return ('[%d%s]' % (n, e), sz * n, al, h * n if h else 0, hc)
    if k == K['Record']:
        decl = typeDecl(t)
        s = cstr(spelling(decl)) or b''
        name = s.decode() or '?'
        parts = []; state = dict(hfa=0, hc='', ok=True, first=True)
        def field(cur, _):
            e, sz, al, h, hc = enc(curType(cur))
            parts.append(e)
            if h and (state['first'] or hc == state['hc']): state['hc'] = hc; state['hfa'] += h
            else: state['ok'] = False
            state['first'] = False
            return 1
        visitFields(t, FIELDV(field), None)
        size, align = sizeOf(t), alignOf(t)
        hfa = state['hfa'] if state['ok'] and state['hfa'] <= 4 else 0
        return ('{%s=%s}' % (name, ''.join(parts)), size, align, hfa, state['hc'] if hfa else '')
    raise Unsupported(k)

def needs_marshal(e, size, hfa, hc):
    if not e.startswith('{'): return False
    if size > 16: return True
    if hfa and hc == 'f': return True
    if hfa and hc == 'd': return False                # <=16 bytes of doubles: v regs <-> xmm regs
    if '{' not in e[1:] and all(c in 'cCsSiIqQB^@' for c in e[e.index('=') + 1:-1]): return False   # plain ints
    return True                                       # mixed int/float aggregates

UMBRELLA = ('#import <Cocoa/Cocoa.h>\n#import <CoreText/CoreText.h>\n#import <ImageIO/ImageIO.h>\n'
            '#import <QuartzCore/QuartzCore.h>\n#include <ApplicationServices/ApplicationServices.h>\n'
            # media, GPU and numerics frameworks: CMTime / CMTimeRange (24 / 48 bytes: by reference on arm64, on the stack on x86_64), AudioStreamBasicDescription (40 bytes),
            # vImage_Buffer (32), CVTime, MTLSize / MTLRegion, ...
            '#if __has_include(<AVFoundation/AVFoundation.h>)\n#import <AVFoundation/AVFoundation.h>\n#endif\n'
            '#import <CoreMedia/CoreMedia.h>\n#import <CoreVideo/CoreVideo.h>\n#import <AudioToolbox/AudioToolbox.h>\n#import <CoreAudio/CoreAudio.h>\n'
            '#import <VideoToolbox/VideoToolbox.h>\n#import <Accelerate/Accelerate.h>\n#import <Metal/Metal.h>\n#import <MetalKit/MetalKit.h>\n'
            '#import <MetalPerformanceShaders/MetalPerformanceShaders.h>\n#import <CoreImage/CoreImage.h>\n#import <IOSurface/IOSurface.h>\n'
            '#if __has_include(<Vision/Vision.h>)\n#import <Vision/Vision.h>\n#endif\n#if __has_include(<CoreML/CoreML.h>)\n#import <CoreML/CoreML.h>\n#endif\n'
            '#if __has_include(<SceneKit/SceneKit.h>)\n#import <SceneKit/SceneKit.h>\n#endif\n#if __has_include(<CoreMIDI/CoreMIDI.h>)\n#import <CoreMIDI/CoreMIDI.h>\n#endif\n'
            '#if __has_include(<ScreenCaptureKit/ScreenCaptureKit.h>)\n#import <ScreenCaptureKit/ScreenCaptureKit.h>\n#endif\n')
# Narrow-result functions also come from the C library and system headers.
UMBRELLA_ALL = UMBRELLA + ('#include <os/lock.h>\n#include <os/log.h>\n#include <pthread.h>\n#include <dispatch/dispatch.h>\n'
                           '#include <stdlib.h>\n#include <string.h>\n#include <unistd.h>\n#include <signal.h>\n#include <dlfcn.h>\n'
                           '#include <mach/mach.h>\n#include <mach/mach_vm.h>\n#include <libkern/OSAtomic.h>\n#include <sys/sysctl.h>\n'
                           '#include <objc/runtime.h>\n#include <CoreFoundation/CoreFoundation.h>\n#include <Security/Security.h>\n'
                           '#include <IOKit/IOKitLib.h>\n#include <SystemConfiguration/SystemConfiguration.h>\n')

def parse_tu(out, text):
    src = os.path.join(os.path.dirname(out), 'csigs_umbrella.m')
    open(src, 'w').write(text)
    sdk = subprocess.check_output(['xcrun', '--show-sdk-path']).decode().strip()
    args = (ctypes.c_char_p * 8)(b'-x', b'objective-c', b'-arch', b'x86_64', b'-isysroot', sdk.encode(), b'-fmodules', b'-w')
    tu = parse(createIndex(0, 0), src.encode(), args, 7, None, 0, 0)
    if not tu: sys.exit('parse failed')
    return tu

def main(out):
    found, narrow = {}, {}
    def visitor(cur, parent, _):
        if curKind(cur) == FUNCTION_DECL and storage(cur) != STATIC:
            ft = curType(cur)
            if variadic(ft): return 1
            nm = cstr(spelling(cur)).decode()
            try:
                r = enc(resType(ft)); parts = [r[0]]; flag = needs_marshal(r[0], r[1], r[3], r[4])
                for i in range(numArgs(cur)):
                    a = enc(curType(getArg(cur, i))); parts.append(a[0])
                    flag = flag or needs_marshal(a[0], a[1], a[3], a[4])
            except Unsupported:
                return 1
            if flag: found['_' + nm] = ''.join(parts)
        return 1
    visit(tuCursor(parse_tu(out, UMBRELLA)), VISITOR(visitor), None)

    def narrow_visitor(cur, parent, _):
        if curKind(cur) == FUNCTION_DECL and storage(cur) != STATIC:
            k = canon(resType(curType(cur))).kind
            code = {K['Bool']: 'B', K['Char_U']: 'C', K['UChar']: 'C', K['Char_S']: 'c', K['SChar']: 'c',
                    K['UShort']: 'S', K['Short']: 's'}.get(k)
            if code: narrow['_' + cstr(spelling(cur)).decode()] = code
        return 1
    stackargs = {}
    def stack_visitor(cur, parent, _):
        # Arguments beyond the 8 integer registers go to the stack. arm64 (Apple) packs them by natural size,
        # x86_64 gives each an 8-byte slot, so a function with small stack arguments needs them re-slotted.
        if curKind(cur) == FUNCTION_DECL and storage(cur) != STATIC:
            ft = curType(cur)
            if variadic(ft): return 1
            ngrn, sizes = 0, []
            try:
                for i in range(numArgs(cur)):
                    e, size = enc(curType(getArg(cur, i)))[:2]
                    if e[0] in '{[' or e in ('f', 'd', 'v'): raise Unsupported(e)
                    if ngrn < 8: ngrn += 1
                    else: sizes.append(size)
            except Unsupported:
                return 1
            if sizes and min(sizes) < 8: stackargs['_' + cstr(spelling(cur)).decode()] = ''.join(str(z) for z in sizes)
        return 1
    tu_all = parse_tu(out, UMBRELLA_ALL)
    visit(tuCursor(tu_all), VISITOR(narrow_visitor), None)
    visit(tuCursor(tu_all), VISITOR(stack_visitor), None)
    with open(out, 'w') as f:
        f.write('// Generated by tools/gen_csigs.py from the macOS SDK headers; do not edit.\n')
        f.write('// C functions whose by-value struct arguments/results need re-marshalling between arm64 and x86_64.\n')
        f.write('static const struct { const char *name; const char *enc; } k_csigs[] = {\n')
        for nm in sorted(found): f.write('    {"%s", "%s"},\n' % (nm, found[nm].replace('\\', '\\\\').replace('"', '\\"')))
        f.write('    {0, 0}\n};\n')
        f.write('// C functions returning bool/char/short: the x86_64 callee leaves the upper bits of rax undefined, the\n')
        f.write('// arm64 caller expects a value extended to 32 bits (B, C: zero; c, s: sign; S: zero).\n')
        f.write('static const struct { const char *name; char kind; } k_narrow[] = {\n')
        for nm in sorted(narrow): f.write('    {"%s", \'%s\'},\n' % (nm, narrow[nm]))
        f.write('    {0, 0}\n};\n')
        f.write('// C functions with stack-passed arguments narrower than 8 bytes: the sizes (in bytes) of those arguments in order.\n')
        f.write('static const struct { const char *name; const char *sizes; } k_stackargs[] = {\n')
        for nm in sorted(stackargs): f.write('    {"%s", "%s"},\n' % (nm, stackargs[nm]))
        f.write('    {0, 0}\n};\n')
    print('wrote %d signatures, %d narrow results and %d stack layouts to %s' % (len(found), len(narrow), len(stackargs), out))

main(sys.argv[1] if len(sys.argv) > 1 else 'csigs.h')
