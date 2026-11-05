import time
def fib(n): return n if n < 2 else fib(n-1) + fib(n-2)
t=time.time(); fib(26); a=time.time()-t
t=time.time(); s=0
for i in range(2000000): s += i*i % 7
b=time.time()-t
t=time.time(); d={str(i):i for i in range(300000)}; l=sorted(d, key=len); c=time.time()-t
print("fib %.2f loop %.2f dict %.2f"%(a,b,c))
