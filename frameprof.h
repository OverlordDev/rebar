// REBAR_FRAMEPROF=<prefix> (or =1 for /tmp/rebar_frames): a frame profiler built into the bridge. Every frame is recorded, none is sampled or dropped.
//
// A frame ends at a present, whichever thread makes it (a game that renders with jobs presents from a different thread every time, so nothing here is per thread): the calls
// -[MTLCommandBuffer presentDrawable:] (also atTime: / afterMinimumDuration:) and -[CAMetalDrawable present...].  Per frame it keeps
//   interval   wall time since the previous present
//   drawable   time threads spent blocked in -[CAMetalLayer nextDrawable] (the display or the GPU is behind: back pressure)
//   cpu        CPU time of the whole process in that interval (all threads); cpu / interval = cores busy
//   host       time spent inside imports (every call from the guest into the host: Metal / driver, libc ...) summed over all threads, plain waits taken out
//   wait       time threads spent in plain waits (condition variables, semaphores, sleeps, locks) summed over all threads: threads with nothing to do
//   guest      cpu - host: an estimate of the CPU spent in the game's own arm64 code under rebar (JIT / interpreter) and in rebar itself
//   gpu        (REBAR_FRAMEPROF_GPU=1) GPU busy time of the command buffers that completed in the interval, from -[MTLCommandBuffer GPUStartTime / GPUEndTime]
//   cb         command buffers committed
// <prefix>.<pid>.csv gets one row per frame (appended once a second: nothing is lost), <prefix>.<pid>.txt is rewritten once a second: fps and frame time percentiles of the last 10 s and of
// the whole run, where an average frame goes, and what the slow frames (> 1.5x the median or > 50 ms) were limited by: the GPU, a drawable wait, busy CPU, or none (threads idle:
// serialisation, latency).
// Cost: two clock reads per import call on every thread, a few per frame; REBAR_FRAMEPROF_GPU=1 adds a completion handler to every command buffer.
#include <time.h>
static int g_fp;                       // 1 when the frame profiler is on
static int g_fp_gpu;                   // 1 when GPU times are collected
static const char *g_fp_prefix;
typedef struct { uint64_t t_ns; uint32_t interval_us, drawable_us, cpu_us, host_us, wait_us, gpu_us; uint16_t ncb; } FpRec;
#define FP_RING (1u << 19)
static FpRec *g_fp_ring;               // ring of frames; the writer thread drains it once a second
static uint64_t g_fp_nframes, g_fp_ncommit, g_fp_start_ns, g_fp_last_present, g_fp_last_commit, g_fp_last_cpu;   // (atomic where threads share them)
static uint64_t g_fp_host_ns, g_fp_wait_ns, g_fp_draw_ns, g_fp_gpu_ns, g_fp_gpu_cbs, g_fp_gpu_zero;
static __thread int t_fp_depth;
static uint8_t *g_fp_kind;             // per import: 0 unknown, 1 other, 2 plain wait
static SEL s_fp_next, s_fp_pdraw, s_fp_pdraw_at, s_fp_pdraw_after, s_fp_present, s_fp_present_at, s_fp_present_after, s_fp_commit, s_fp_addcomp, s_fp_gs, s_fp_ge;
static void *fp_msg(void) { static void *p; if (!p) p = dlsym(RTLD_DEFAULT, "objc_msgSend"); return p; }
static uint64_t fp_now(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static uint64_t fp_proc_cpu(void) { return clock_gettime_nsec_np(CLOCK_PROCESS_CPUTIME_ID); }
static int fp_stub_kind(uint64_t idx) {
    if (idx >= 65536) return 1;
    uint8_t k = g_fp_kind[idx];
    if (k) return k;
    static const char *const waits[] = {"_pthread_cond_wait", "_pthread_cond_timedwait", "_semaphore_wait", "_semaphore_timedwait", "_dispatch_semaphore_wait", "_dispatch_group_wait", "_usleep", "_nanosleep",
        "_sleep", "_os_sync_wait_on_address", "__ulock_wait", "_pthread_mutex_lock", "_pthread_join", "_mach_msg", "_kevent", "_select", "_poll", "_pthread_rwlock_rdlock", "_pthread_rwlock_wrlock", "_flock", "_objc_sync_enter"};
    k = 1;
    if (idx < (uint64_t)g_nstubs && g_stubs[idx].name) for (size_t i = 0; i < sizeof waits / sizeof waits[0]; i++) if (!strncmp(g_stubs[idx].name, waits[i], strlen(waits[i]))) { k = 2; break; }
    g_fp_kind[idx] = k;
    return k;
}
static void fp_frame(void) {   // a present, from any thread: close the frame
    uint64_t now = fp_now(), prev = __atomic_exchange_n(&g_fp_last_present, now, __ATOMIC_ACQ_REL);
    if (!prev) { g_fp_last_cpu = fp_proc_cpu(); g_fp_last_commit = __atomic_load_n(&g_fp_ncommit, __ATOMIC_RELAXED); return; }
    uint64_t cpu = fp_proc_cpu(), cpu0 = __atomic_exchange_n(&g_fp_last_cpu, cpu, __ATOMIC_ACQ_REL);
    uint64_t commits = __atomic_load_n(&g_fp_ncommit, __ATOMIC_RELAXED), c0 = __atomic_exchange_n(&g_fp_last_commit, commits, __ATOMIC_ACQ_REL);
    uint64_t host = __atomic_exchange_n(&g_fp_host_ns, 0, __ATOMIC_ACQ_REL), wait = __atomic_exchange_n(&g_fp_wait_ns, 0, __ATOMIC_ACQ_REL);
    uint64_t draw = __atomic_exchange_n(&g_fp_draw_ns, 0, __ATOMIC_ACQ_REL), gpu = __atomic_exchange_n(&g_fp_gpu_ns, 0, __ATOMIC_ACQ_REL);
    uint64_t n = __atomic_fetch_add(&g_fp_nframes, 1, __ATOMIC_RELAXED);
    FpRec *r = &g_fp_ring[n & (FP_RING - 1)];
    uint64_t busy = host > wait ? host - wait : 0;
    r->t_ns = now - g_fp_start_ns; r->interval_us = (uint32_t)((now - prev) / 1000); r->drawable_us = (uint32_t)(draw / 1000); r->cpu_us = (uint32_t)((cpu - cpu0) / 1000);
    r->host_us = (uint32_t)(busy / 1000); r->wait_us = (uint32_t)(wait / 1000); r->gpu_us = (uint32_t)(gpu / 1000); r->ncb = (uint16_t)(commits - c0 > 65535 ? 65535 : commits - c0);
}
static int fp_cmp_u32(const void *a, const void *b) { uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b; return x < y ? -1 : x > y; }
static double fp_pct(uint32_t *sorted, size_t n, double p) { return n ? sorted[(size_t)((n - 1) * p)] / 1000.0 : 0; }
static void fp_summary(FILE *f, const char *title, const FpRec *r, size_t n) {
    if (n < 2) { fprintf(f, "%s: no frames\n", title); return; }
    uint32_t *iv = malloc(n * 4); uint64_t sd = 0, sc = 0, sh = 0, sw = 0, sg = 0, scb = 0, siv = 0; size_t over33 = 0, over50 = 0, over100 = 0;
    for (size_t i = 0; i < n; i++) { iv[i] = r[i].interval_us; siv += r[i].interval_us; sd += r[i].drawable_us; sc += r[i].cpu_us; sh += r[i].host_us; sw += r[i].wait_us; sg += r[i].gpu_us; scb += r[i].ncb;
        over33 += r[i].interval_us > 33300; over50 += r[i].interval_us > 50000; over100 += r[i].interval_us > 100000; }
    uint64_t span_us = r[n - 1].t_ns / 1000 - r[0].t_ns / 1000; double fps = span_us ? (double)(n - 1) * 1e6 / (double)span_us : 0;
    qsort(iv, n, 4, fp_cmp_u32);
    double med = fp_pct(iv, n, 0.5), avg = (double)siv / n / 1000, N = (double)n;
    fprintf(f, "%s: %zu frames, %.1f fps, frame time avg %.1f  p50 %.1f  p95 %.1f  p99 %.1f  max %.1f ms\n", title, n, fps, avg, med, fp_pct(iv, n, 0.95), fp_pct(iv, n, 0.99), iv[n - 1] / 1000.0);
    fprintf(f, "  frames over 33.3 ms: %zu (%.0f %%)   over 50 ms: %zu   over 100 ms: %zu   command buffers per frame %.1f\n", over33, 100.0 * over33 / n, over50, over100, (double)scb / N);
    double cpu_ms = (double)sc / N / 1000, host_ms = (double)sh / N / 1000;
    fprintf(f, "  an average frame of %.1f ms: process cpu %.1f ms = %.1f cores busy (of which in host calls: Metal / driver / libc %.1f ms, guest code under rebar and rebar itself about %.1f ms)\n",
            avg, cpu_ms, cpu_ms / avg, host_ms, cpu_ms > host_ms ? cpu_ms - host_ms : 0);
    fprintf(f, "  threads idle in plain waits %.1f thread-ms per frame, blocked in nextDrawable %.1f ms per frame", (double)sw / N / 1000, (double)sd / N / 1000);
    if (g_fp_gpu) fprintf(f, ", GPU busy %.1f ms per frame = %.0f %% of the frame (%.1f %% of command buffers reported no GPU time)", (double)sg / N / 1000, 100.0 * (double)sg / (double)siv, g_fp_gpu_cbs ? 100.0 * (double)g_fp_gpu_zero / (double)g_fp_gpu_cbs : 0);
    fprintf(f, "\n");
    size_t ng = 0, nd = 0, nc = 0, ni = 0, ns = 0;
    for (size_t i = 0; i < n; i++) {
        if (!(r[i].interval_us > 50000 || r[i].interval_us > med * 1500)) continue;
        ns++; double iv_us = r[i].interval_us;
        if (g_fp_gpu && r[i].gpu_us >= 0.8 * iv_us) ng++; else if (r[i].drawable_us >= 0.5 * iv_us) nd++; else if (r[i].cpu_us >= 3.0 * iv_us) nc++; else ni++;
    }
    fprintf(f, "  slow frames (> 1.5 x median or > 50 ms): %zu = GPU-bound (GPU busy >= 80 %%) %zu, blocked in nextDrawable (>= 50 %%) %zu, CPU-heavy (>= 3 cores) %zu, neither (idle threads: serialisation / latency) %zu\n", ns, ng, nd, nc, ni);
    free(iv);
}
static void *fp_writer(void *arg) {
    (void)arg;
    char csv[PATH_MAX], txt[PATH_MAX], tmp[PATH_MAX];
    snprintf(csv, sizeof csv, "%s.%d.csv", g_fp_prefix, (int)getpid()); snprintf(txt, sizeof txt, "%s.%d.txt", g_fp_prefix, (int)getpid()); snprintf(tmp, sizeof tmp, "%s.tmp", txt);
    FILE *fc = fopen(csv, "w"); if (fc) fprintf(fc, "frame,t_s,interval_ms,drawable_ms,cpu_ms,host_ms,wait_ms,gpu_ms,cmdbufs\n");
    uint64_t done = 0; FpRec *all = NULL; size_t nall = 0, capall = 0;
    for (;;) {
        sleep(1);
        uint64_t n = __atomic_load_n(&g_fp_nframes, __ATOMIC_ACQUIRE);
        if (n - done > FP_RING) done = n - FP_RING;   // (the writer fell behind by more than the ring: only then frames are lost)
        for (; done < n; done++) {
            const FpRec *r = &g_fp_ring[done & (FP_RING - 1)];
            if (fc) fprintf(fc, "%llu,%.3f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%u\n", (unsigned long long)done, r->t_ns / 1e9, r->interval_us / 1e3, r->drawable_us / 1e3, r->cpu_us / 1e3, r->host_us / 1e3, r->wait_us / 1e3, r->gpu_us / 1e3, (unsigned)r->ncb);
            if (nall == capall) { capall = capall ? capall * 2 : 65536; all = realloc(all, capall * sizeof(FpRec)); }
            all[nall++] = *r;
        }
        if (fc) fflush(fc);
        FILE *f = fopen(tmp, "w"); if (!f) continue;
        uint64_t tnow = fp_now() - g_fp_start_ns; size_t k = nall;
        while (k > 0 && tnow - all[k - 1].t_ns < 10ull * 1000000000ull) k--;
        fprintf(f, "rebar frame profiler, pid %d, running %.0f s (csv: %s)\n", (int)getpid(), tnow / 1e9, csv);
        fp_summary(f, "last 10 s", all + k, nall - k);
        fp_summary(f, "whole run", all, nall);
        if (nall) {   // the eight worst frames
            size_t idx[8] = {0}; int m = 0;
            for (size_t i = 0; i < nall; i++) { if (m < 8) { idx[m++] = i; } else { int lo = 0; for (int q = 1; q < 8; q++) if (all[idx[q]].interval_us < all[idx[lo]].interval_us) lo = q; if (all[i].interval_us > all[idx[lo]].interval_us) idx[lo] = i; } }
            fprintf(f, "worst frames (t s: interval | drawable cpu host wait gpu, ms):\n");
            for (int q = 0; q < m; q++) { const FpRec *r = &all[idx[q]]; fprintf(f, "  %7.2f: %7.1f | %6.1f %7.1f %6.1f %7.1f %6.1f\n", r->t_ns / 1e9, r->interval_us / 1e3, r->drawable_us / 1e3, r->cpu_us / 1e3, r->host_us / 1e3, r->wait_us / 1e3, r->gpu_us / 1e3); }
        }
        fclose(f); rename(tmp, txt);
    }
    return NULL;
}
static void frameprof_start(const char *prefix) {
    g_fp_prefix = (!prefix || !strcmp(prefix, "1")) ? "/tmp/rebar_frames" : strdup(prefix);
    g_fp_ring = calloc(FP_RING, sizeof(FpRec)); g_fp_kind = calloc(65536, 1); g_fp_start_ns = fp_now();
    g_fp_gpu = getenv("REBAR_FRAMEPROF_GPU") != NULL;
#define FPSEL(v, s) v = sel_registerName(s)
    FPSEL(s_fp_next, "nextDrawable"); FPSEL(s_fp_pdraw, "presentDrawable:"); FPSEL(s_fp_pdraw_at, "presentDrawable:atTime:"); FPSEL(s_fp_pdraw_after, "presentDrawable:afterMinimumDuration:");
    FPSEL(s_fp_present, "present"); FPSEL(s_fp_present_at, "presentAtTime:"); FPSEL(s_fp_present_after, "presentAfterMinimumDuration:"); FPSEL(s_fp_commit, "commit");
    FPSEL(s_fp_addcomp, "addCompletedHandler:"); FPSEL(s_fp_gs, "GPUStartTime"); FPSEL(s_fp_ge, "GPUEndTime");
    g_fp = 1;
    pthread_t th; pthread_create(&th, NULL, fp_writer, NULL); pthread_detach(th);
    fprintf(stderr, "rebar: frame profiler on%s: %s.%d.txt (summary, once a second) and .csv (every frame)\n", g_fp_gpu ? " (with GPU times)" : "", g_fp_prefix, (int)getpid());
}
// A command buffer finished on the GPU: its busy time goes into the frame that is open now.
static void fp_cb_done(id cb) {
    double s = ((double (*)(id, SEL))fp_msg())(cb, s_fp_gs), e = ((double (*)(id, SEL))fp_msg())(cb, s_fp_ge);
    __atomic_fetch_add(&g_fp_gpu_cbs, 1, __ATOMIC_RELAXED);
    if (e > s && s > 0) __atomic_fetch_add(&g_fp_gpu_ns, (uint64_t)((e - s) * 1e9), __ATOMIC_RELAXED); else __atomic_fetch_add(&g_fp_gpu_zero, 1, __ATOMIC_RELAXED);
}
static void stub_call_plain(CPU *c);
// Called instead of the plain stub_call while the profiler is on.
static void fp_stub_call(CPU *c) {
    uint64_t off = c->pc - g_stub_base, idx = off / 16;
    bool valid = off < g_stub_size && idx < (uint64_t)g_nstubs;
    bool is_next = false;
    if (valid && g_stubs[idx].handler == h_objc_msgSend) {
        SEL sel = (SEL)c->x[1]; id self = (id)c->x[0];
        if (sel == s_fp_next) is_next = true;
        else if (sel == s_fp_commit) {
            if (self && strstr(class_getName(object_getClass(self)), "CommandBuffer")) {
                __atomic_fetch_add(&g_fp_ncommit, 1, __ATOMIC_RELAXED);
                if (g_fp_gpu && !t_fp_depth) { t_fp_depth++; ((void (*)(id, SEL, void *))fp_msg())(self, s_fp_addcomp, (void *)^(id cb) { fp_cb_done(cb); }); t_fp_depth--; }
            }
        }
        else if (sel == s_fp_pdraw || sel == s_fp_pdraw_at || sel == s_fp_pdraw_after) { if (self) fp_frame(); }
        else if ((sel == s_fp_present || sel == s_fp_present_at || sel == s_fp_present_after) && self && strstr(class_getName(object_getClass(self)), "Drawable")) fp_frame();
    }
    if (t_fp_depth) { stub_call_plain(c); return; }
    int kind = valid ? fp_stub_kind(idx) : 1;
    uint64_t a = fp_now();
    t_fp_depth++; stub_call_plain(c); t_fp_depth--;
    uint64_t d = fp_now() - a;
    __atomic_fetch_add(&g_fp_host_ns, d, __ATOMIC_RELAXED);
    if (kind == 2) __atomic_fetch_add(&g_fp_wait_ns, d, __ATOMIC_RELAXED);
    if (is_next) __atomic_fetch_add(&g_fp_draw_ns, d, __ATOMIC_RELAXED);
}
