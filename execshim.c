// librebar_exec.dylib: injected (DYLD_INSERT_LIBRARIES) into the x86_64 programs that an arm64 guest starts under rebar (a login shell, env, a shell script, git hooks),
// so that these can run arm64-only executables as well: the kernel of this Mac refuses them ("bad CPU type in executable"), the shim starts them under rebar instead.
// It replaces execve / execv / execvp / posix_spawn / posix_spawnp: when the target is an arm64-only Mach-O (or a script whose interpreter is), the call becomes
// `rebar <target> args...`. REBAR_BIN names rebar; without it every call goes straight through.
#include <errno.h>
#include <fcntl.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern char **environ;

// 1: a Mach-O that only runs as arm64 (a thin arm64 file, or a universal one with an arm64 slice and no x86_64 one; arm64e needs pointer authentication, which rebar cannot do)
static int arm64_only(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    uint8_t buf[4096];
    ssize_t n = pread(fd, buf, sizeof buf, 0);
    close(fd);
    if (n < 32) return 0;
    uint32_t magic = *(uint32_t *)buf;
    if (magic == MH_MAGIC_64) { const struct mach_header_64 *h = (const void *)buf; return h->cputype == CPU_TYPE_ARM64 && (h->cpusubtype & 0xff) != 2; }
    if (magic == FAT_CIGAM) {
        const struct fat_header *fh = (const void *)buf;
        const struct fat_arch *fa = (const void *)(fh + 1);
        int arm = 0, x86 = 0;
        uint32_t na = __builtin_bswap32(fh->nfat_arch);
        for (uint32_t k = 0; k < na && (const uint8_t *)(fa + k + 1) <= buf + n; k++) {
            cpu_type_t t = (cpu_type_t)__builtin_bswap32(fa[k].cputype);
            if (t == CPU_TYPE_ARM64 && (__builtin_bswap32(fa[k].cpusubtype) & 0xff) != 2) arm = 1;
            if (t == CPU_TYPE_X86_64) x86 = 1;
        }
        return arm && !x86;
    }
    return 0;
}

// The command line rebar needs: [rebar, program, <extra interpreter argument>, script, argv[1..]]. For a script whose "#!" interpreter is arm64-only the interpreter is the program.
static char **rebar_command(const char *rebar, const char *path, char *const *argv) {
    char interp[1024] = "", iarg[1024] = "";
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    int script = 0;
    if (fd >= 0) {
        char head[2048]; ssize_t n = pread(fd, head, sizeof head - 1, 0); close(fd);
        if (n > 2 && head[0] == '#' && head[1] == '!') {
            head[n] = 0; char *nl = strchr(head, '\n'); if (nl) *nl = 0;
            char *p = head + 2; while (*p == ' ' || *p == '\t') p++;
            size_t l = strcspn(p, " \t"); if (l && l < sizeof interp) { memcpy(interp, p, l); interp[l] = 0; p += l; while (*p == ' ' || *p == '\t') p++; if (*p) snprintf(iarg, sizeof iarg, "%s", p); script = 1; }
        }
    }
    int argc = 0; while (argv && argv[argc]) argc++;
    char **v = calloc((size_t)argc + 5, sizeof *v);
    int q = 0;
    v[q++] = (char *)rebar;
    if (script) { v[q++] = strdup(interp); if (iarg[0]) v[q++] = strdup(iarg); v[q++] = (char *)path; }
    else v[q++] = (char *)path;
    for (int k = 1; k < argc; k++) v[q++] = argv[k];
    return v;
}
// 1 when the file (a program, or a script with an arm64-only interpreter) cannot run on this Mac directly
static int needs_rebar(const char *path) {
    if (arm64_only(path)) return 1;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    char head[512]; ssize_t n = pread(fd, head, sizeof head - 1, 0); close(fd);
    if (n > 2 && head[0] == '#' && head[1] == '!') {
        head[n] = 0; char *nl = strchr(head, '\n'); if (nl) *nl = 0;
        char *p = head + 2; while (*p == ' ' || *p == '\t') p++;
        char interp[512]; size_t l = strcspn(p, " \t"); if (!l || l >= sizeof interp) return 0; memcpy(interp, p, l); interp[l] = 0;
        return arm64_only(interp);
    }
    return 0;
}
static const char *find_in_path(const char *file, char *out, size_t cap) {   // the first executable named file in PATH
    if (strchr(file, '/')) { snprintf(out, cap, "%s", file); return out; }
    const char *path = getenv("PATH"); if (!path) path = "/usr/bin:/bin";
    while (*path) {
        const char *e = strchr(path, ':'); size_t l = e ? (size_t)(e - path) : strlen(path);
        snprintf(out, cap, "%.*s/%s", (int)l, path, file);
        struct stat st; if (stat(out, &st) == 0 && S_ISREG(st.st_mode) && access(out, X_OK) == 0) return out;
        path += l + (e ? 1 : 0);
    }
    return NULL;
}

static int my_execve(const char *path, char *const argv[], char *const envp[]) {
    const char *rebar = getenv("REBAR_BIN");
    if (rebar && path && needs_rebar(path)) { char **v = rebar_command(rebar, path, argv); execve(rebar, v, envp); free(v); return -1; }
    return execve(path, argv, envp);
}
static int my_execv(const char *path, char *const argv[]) { return my_execve(path, argv, environ); }
static int my_execvp(const char *file, char *const argv[]) {
    const char *rebar = getenv("REBAR_BIN");
    char full[1024];
    if (rebar && file && find_in_path(file, full, sizeof full) && needs_rebar(full)) { char **v = rebar_command(rebar, full, argv); execve(rebar, v, environ); free(v); return -1; }
    return execvp(file, argv);
}
static int my_posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *fa, const posix_spawnattr_t *attr, char *const argv[], char *const envp[]) {
    const char *rebar = getenv("REBAR_BIN");
    if (rebar && path && needs_rebar(path)) { char **v = rebar_command(rebar, path, argv); int r = posix_spawn(pid, rebar, fa, attr, v, envp); free(v); return r; }
    return posix_spawn(pid, path, fa, attr, argv, envp);
}
static int my_posix_spawnp(pid_t *pid, const char *file, const posix_spawn_file_actions_t *fa, const posix_spawnattr_t *attr, char *const argv[], char *const envp[]) {
    const char *rebar = getenv("REBAR_BIN");
    char full[1024];
    if (rebar && file && find_in_path(file, full, sizeof full) && needs_rebar(full)) { char **v = rebar_command(rebar, full, argv); int r = posix_spawn(pid, rebar, fa, attr, v, envp); free(v); return r; }
    return posix_spawnp(pid, file, fa, attr, argv, envp);
}

#define INTERPOSE(mine, orig) { (void *)(mine), (void *)(orig) }
__attribute__((used)) static struct { void *mine, *orig; } interposers[] __attribute__((section("__DATA,__interpose"))) = {
    INTERPOSE(my_execve, execve), INTERPOSE(my_execv, execv), INTERPOSE(my_execvp, execvp), INTERPOSE(my_posix_spawn, posix_spawn), INTERPOSE(my_posix_spawnp, posix_spawnp),
};
