/* libme1run.so: small arm64 executable for the launcher's test button (Android allows exec() only from the native
 * library folder). Loads libme1game.so from its own folder and runs the game without a window: the GLES backend
 * renders offscreen, frames can be saved with ME1_FRAME_DUMP. Usage as test_exe: <game dir> <docs dir> [image dir] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <dlfcn.h>
#include <limits.h>
static void (*on_sig)(int);
static void sig(int s) { if (on_sig) on_sig(s); _exit(128 + s); }
int main(int argc, char** argv) {
    char self[PATH_MAX]; ssize_t n = readlink("/proc/self/exe", self, sizeof self - 32);
    if (n <= 0) { fprintf(stderr, "[run] readlink /proc/self/exe failed\n"); return 2; }
    self[n] = 0; char* sl = strrchr(self, '/'); strcpy(sl + 1, "libme1game.so");
    void* h = dlopen(self, RTLD_NOW);
    if (!h) { fprintf(stderr, "[run] dlopen %s: %s\n", self, dlerror()); return 2; }
    int (*m)(int, char**) = (int (*)(int, char**))dlsym(h, "me1_exe_main");
    on_sig = (void (*)(int))dlsym(h, "me1_exe_on_signal");
    if (!m) { fprintf(stderr, "[run] me1_exe_main not found\n"); return 2; }
    signal(SIGTERM, sig); signal(SIGINT, sig);
    setvbuf(stdout, NULL, _IOLBF, 0);
    return m(argc, argv);
}
