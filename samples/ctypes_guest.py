import ctypes
# calls into an arm64 (guest) library: struct by value in and out, floats, many arguments, variadic
import _ctypes_test
t = ctypes.CDLL(_ctypes_test.__file__)
class P(ctypes.Structure): _fields_ = [("x", ctypes.c_int), ("y", ctypes.c_int)]
class H(ctypes.Structure): _fields_ = [("a", ctypes.c_uint16)] * 8
t._testfunc_byval.argtypes = (P, ctypes.POINTER(P), ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int)); t._testfunc_byval.restype = ctypes.c_int
src = P(3, 4); dst = P(); gx = ctypes.c_int(); gy = ctypes.c_int()
sz = t._testfunc_byval(src, ctypes.byref(dst), ctypes.byref(gx), ctypes.byref(gy))
t.ReturnRect.restype = ctypes.c_int; 
t._testfunc_f_bhilfd.restype = ctypes.c_float; t._testfunc_f_bhilfd.argtypes = (ctypes.c_byte, ctypes.c_short, ctypes.c_int, ctypes.c_long, ctypes.c_float, ctypes.c_double)
t._testfunc_d_bhilfd.restype = ctypes.c_double; t._testfunc_d_bhilfd.argtypes = t._testfunc_f_bhilfd.argtypes
print("guest", sz, dst.x, dst.y, gx.value, gy.value, t._testfunc_f_bhilfd(1, 2, 3, 4, 5.0, 6.0), t._testfunc_d_bhilfd(1, 2, 3, 4, 5.0, 6.0))
