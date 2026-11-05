import Foundation
import Darwin
// pty
var master: Int32 = 0, slave: Int32 = 0; print("openpty", openpty(&master, &slave, nil, nil, nil))
write(master, "ping\n", 5); var buf = [UInt8](repeating: 0, count: 64); usleep(100000); let n = read(slave, &buf, 64); print("pty read", n, String(decoding: buf.prefix(Int(max(n, 0))), as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines))
var tio = termios(); print("tcgetattr", tcgetattr(slave, &tio), (tio.c_lflag & tcflag_t(ECHO)) != 0)
// kqueue
let kq = kqueue(); var fds: [Int32] = [0, 0]; pipe(&fds)
var ch = kevent(ident: UInt(fds[0]), filter: Int16(EVFILT_READ), flags: UInt16(EV_ADD | EV_ENABLE), fflags: 0, data: 0, udata: nil)
kevent(kq, &ch, 1, nil, 0, nil); write(fds[1], "x", 1); var ev = kevent(); var ts = timespec(tv_sec: 5, tv_nsec: 0); print("kqueue", kevent(kq, nil, 0, &ev, 1, &ts), ev.data)
var tch = kevent(ident: 1, filter: Int16(EVFILT_TIMER), flags: UInt16(EV_ADD), fflags: 0, data: 20, udata: nil); kevent(kq, &tch, 1, nil, 0, nil); print("kq timer", kevent(kq, nil, 0, &ev, 1, &ts), ev.filter == Int16(EVFILT_TIMER))
let dpath = NSTemporaryDirectory() + "rebar_kq_\(getpid())"; mkdir(dpath, 0o755); let dfd = open(dpath, O_EVTONLY)
var vch = kevent(ident: UInt(dfd), filter: Int16(EVFILT_VNODE), flags: UInt16(EV_ADD | EV_CLEAR), fflags: UInt32(NOTE_WRITE), data: 0, udata: nil); kevent(kq, &vch, 1, nil, 0, nil)
FileManager.default.createFile(atPath: dpath + "/f", contents: Data()); ts.tv_sec = 5; print("kq vnode", kevent(kq, nil, 0, &ev, 1, &ts)); try? FileManager.default.removeItem(atPath: dpath)
// mmap / shm / madvise
let shm = NSTemporaryDirectory() + "rebar_shm_\(getpid())"; let sfd = open(shm, O_CREAT | O_RDWR, 0o600); ftruncate(sfd, 4096)
let p = mmap(nil, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, sfd, 0)!; p.storeBytes(of: 0x1234_5678, as: UInt32.self)
let p2 = mmap(nil, 4096, PROT_READ, MAP_SHARED, sfd, 0)!; print("shm", String(p2.load(as: UInt32.self), radix: 16), madvise(p, 4096, MADV_WILLNEED), msync(p, 4096, MS_SYNC)); unlink(shm)
var mincore_v = [CChar](repeating: 0, count: 1); print("mincore", mincore(p, 4096, &mincore_v))
// fork/exec/wait
var pid: pid_t = 0; var attr: posix_spawn_file_actions_t?; posix_spawn_file_actions_init(&attr)
let argv: [UnsafeMutablePointer<CChar>?] = [strdup("/bin/echo"), strdup("spawned"), nil]; posix_spawn(&pid, "/bin/echo", &attr, nil, argv, environ); var st: Int32 = 0; waitpid(pid, &st, 0); print("spawn", st)
// getifaddrs / sysctl / proc
var ifa: UnsafeMutablePointer<ifaddrs>?; getifaddrs(&ifa); var names = Set<String>(); var p3 = ifa; while let i = p3 { names.insert(String(cString: i.pointee.ifa_name)); p3 = i.pointee.ifa_next }; freeifaddrs(ifa); print("ifaddrs", names.contains("lo0"))
var sz = 0; sysctlbyname("hw.memsize", nil, &sz, nil, 0); var mem: UInt64 = 0; sz = 8; sysctlbyname("hw.memsize", &mem, &sz, nil, 0); print("sysctl", mem > 1 << 30)
var kp = kinfo_proc(); var len = MemoryLayout<kinfo_proc>.size; var mib: [Int32] = [CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()]; sysctl(&mib, 4, &kp, &len, nil, 0); print("kinfo_proc", kp.kp_proc.p_pid == getpid())
var pbi = proc_bsdinfo(); print("proc_pidinfo", proc_pidinfo(getpid(), PROC_PIDTBSDINFO, 0, &pbi, Int32(MemoryLayout<proc_bsdinfo>.size)) > 0)
var tbi = proc_taskinfo(); print("taskinfo", proc_pidinfo(getpid(), PROC_PIDTASKINFO, 0, &tbi, Int32(MemoryLayout<proc_taskinfo>.size)) > 0, tbi.pti_threadnum > 0)
var ru = rusage(); getrusage(RUSAGE_SELF, &ru); print("rusage", ru.ru_maxrss > 0)
var rl = rlimit(); getrlimit(RLIMIT_NOFILE, &rl); print("rlimit", rl.rlim_cur > 100)
// DNS / sockets
var hints = addrinfo(); hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM; var res: UnsafeMutablePointer<addrinfo>?; print("getaddrinfo", getaddrinfo("localhost", "80", &hints, &res), res != nil)
let us = socket(AF_UNIX, SOCK_DGRAM, 0); var pair: [Int32] = [0, 0]; socketpair(AF_UNIX, SOCK_STREAM, 0, &pair); write(pair[0], "sp", 2); var sb = [UInt8](repeating: 0, count: 4); print("socketpair", read(pair[1], &sb, 4), us > 0)
let udp = socket(AF_INET, SOCK_DGRAM, 0); var a = sockaddr_in(); a.sin_family = sa_family_t(AF_INET); a.sin_addr.s_addr = inet_addr("127.0.0.1"); a.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
withUnsafePointer(to: &a) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { _ = bind(udp, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) } }
var alen = socklen_t(MemoryLayout<sockaddr_in>.size); var bound = sockaddr_in(); withUnsafeMutablePointer(to: &bound) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { _ = getsockname(udp, $0, &alen) } }
let cs = socket(AF_INET, SOCK_DGRAM, 0); withUnsafePointer(to: &bound) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { _ = sendto(cs, "udp!", 4, 0, $0, alen) } }; var ub = [UInt8](repeating: 0, count: 8); print("udp", recv(udp, &ub, 8, 0))
// setjmp-ish / TLS / threads
var key = pthread_key_t(); pthread_key_create(&key, nil); pthread_setspecific(key, UnsafeRawPointer(bitPattern: 77)); var thr = pthread_t(bitPattern: 0); let tsem = DispatchSemaphore(value: 0); var tlsSeen = true
pthread_create(&thr, nil, { a in _ = a; return nil }, nil); pthread_join(thr!, nil); print("tls", Int(bitPattern: pthread_getspecific(key)))
var tv = timeval(); gettimeofday(&tv, nil); var mach = mach_timebase_info(); mach_timebase_info(&mach); print("time", tv.tv_sec > 1_700_000_000, mach.denom > 0, mach_absolute_time() > 0)
var t = tm(); var tt = time_t(0); gmtime_r(&tt, &t); print("gmtime", t.tm_year, t.tm_mday); setlocale(LC_ALL, "en_US.UTF-8"); print("strftime", { var b = [CChar](repeating: 0, count: 32); strftime(&b, 32, "%Y-%m-%d", &t); return String(cString: b) }())
print("flock", { let fd = open(dpath + "x", O_CREAT | O_RDWR, 0o600); defer { close(fd); unlink(dpath + "x") }; return (flock(fd, LOCK_EX | LOCK_NB), fcntl(fd, F_GETFL) >= 0) }())
print("xattr", { let f = NSTemporaryDirectory() + "rebar_xa_\(getpid())"; FileManager.default.createFile(atPath: f, contents: Data()); defer { unlink(f) }; setxattr(f, "user.rebar", "v", 1, 0, 0); var b = [CChar](repeating: 0, count: 8); return (getxattr(f, "user.rebar", &b, 8, 0, 0), listxattr(f, nil, 0, 0) > 0) }())
print("clonefile", { let f = NSTemporaryDirectory() + "rebar_cl_\(getpid())"; FileManager.default.createFile(atPath: f, contents: Data("data".utf8)); defer { unlink(f); unlink(f + "2") }; return (clonefile(f, f + "2", 0), (try? String(contentsOfFile: f + "2", encoding: .utf8)) ?? "?") }())
