// The x86_64 half of a wrapped app (see tools/rebar-wrap): the app's main executable becomes a universal file with this program as its x86_64 slice next to the original arm64
// slice. The kernel of this Mac runs the x86_64 slice (it cannot run the arm64 one), so a double click in Finder, the Dock, `open` and a file association all start this program,
// which replaces itself with `rebar <this file> args...`: rebar reads the arm64 slice of the very same file. Same pid, same bundle, same Info.plist: nothing else about the app changes.
// rebar is found by $REBAR_BIN, then the first line of ~/Library/Application Support/rebar/rebar-path (written by rebar-wrap), then /usr/local/bin/rebar.
#include <errno.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// (tools/rebar-wrap recognises a wrapped executable by this string)
__attribute__((used)) static const char rebar_wrapped_marker[] = "REBAR-WRAPPED-1";

int main(int argc, char **argv) {
    char self[PATH_MAX], real[PATH_MAX]; uint32_t n = sizeof self;
    if (_NSGetExecutablePath(self, &n) != 0 || !realpath(self, real)) { fprintf(stderr, "rebar shim: cannot find itself\n"); return 70; }
    char rebar[PATH_MAX] = "";
    const char *e = getenv("REBAR_BIN");
    if (e && *e) snprintf(rebar, sizeof rebar, "%s", e);
    if (!rebar[0]) {
        const char *home = getenv("HOME"); char cfg[PATH_MAX];
        if (home) { snprintf(cfg, sizeof cfg, "%s/Library/Application Support/rebar/rebar-path", home); FILE *f = fopen(cfg, "r"); if (f) { if (fgets(rebar, sizeof rebar, f)) rebar[strcspn(rebar, "\r\n")] = 0; fclose(f); } }
    }
    if (!rebar[0] || access(rebar, X_OK) != 0) snprintf(rebar, sizeof rebar, "/usr/local/bin/rebar");
    if (access(rebar, X_OK) != 0) { fprintf(stderr, "rebar shim: rebar not found (set REBAR_BIN, or run tools/rebar-wrap setup)\n"); return 71; }
    if (!getenv("MallocNanoZone")) setenv("MallocNanoZone", "0", 1);
    {   // per-app environment: ~/Library/Application Support/rebar/env/<name of this executable>, one KEY=VALUE per line (what a launch from Finder or Steam does not have: Steam's ids, rebar switches)
        const char *home = getenv("HOME"); const char *slash = strrchr(real, '/');
        if (home && slash) {
            char path[PATH_MAX]; snprintf(path, sizeof path, "%s/Library/Application Support/rebar/env/%s", home, slash + 1);
            FILE *f = fopen(path, "r");
            if (f) {
                char line[2048];
                while (fgets(line, sizeof line, f)) {
                    line[strcspn(line, "\r\n")] = 0;
                    char *eq = strchr(line, '=');
                    if (line[0] == '#' || !eq || eq == line) continue;
                    *eq = 0; setenv(line, eq + 1, 0);   // (what the caller already set wins: Steam's own SteamAppId, say)
                }
                fclose(f);
            }
        }
    }
    char **nv = calloc((size_t)argc + 3, sizeof *nv); int k = 0;
    nv[k++] = rebar; nv[k++] = real;
    for (int i = 1; i < argc; i++) nv[k++] = argv[i];
    execv(rebar, nv);
    fprintf(stderr, "rebar shim: cannot start %s: %s\n", rebar, strerror(errno));
    return 72;
}
