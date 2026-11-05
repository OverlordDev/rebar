# ctypes through libffi: scalar calls into libSystem / libm, a qsort callback (a libffi closure), a pointer round trip
import ctypes, ctypes.util
libc = ctypes.CDLL(None)
libc.strlen.restype = ctypes.c_size_t
libm = ctypes.CDLL(ctypes.util.find_library("m")); libm.pow.restype = ctypes.c_double; libm.pow.argtypes = [ctypes.c_double, ctypes.c_double]
arr = (ctypes.c_int * 5)(5, 1, 4, 2, 3); libc.qsort.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_size_t, ctypes.c_void_p]
CMP = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int))
libc.qsort(arr, 5, 4, CMP(lambda a, b: a[0] - b[0]))
buf = ctypes.create_string_buffer(b"42-x", 16)
print("ctypes", libc.strlen(b"hello"), list(arr), libm.pow(2, 10), libc.atoi(buf), buf.value.decode())
