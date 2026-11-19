// Fuzz-only helper: when $FUZZ_LEAKCHECK_FILE appears, run LSan from a normal thread and exit.
// Only activates in processes that contain the LSan runtime; strips LD_PRELOAD for children.
#define _GNU_SOURCE
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>
#include <stdio.h>
static int (*chk)(void);
static char path[4096];
static void *watch(void *a) {
    for (;;) {
        if (access(path, F_OK) == 0) {
            int r = chk();
            FILE *o = fopen(path, "a"); if (o) { fprintf(o, "leakcheck=%d\n", r); fclose(o); }
            _exit(r ? 23 : 0);
        }
        usleep(300000);
    }
}
__attribute__((constructor)) static void init(void) {
    const char *f = getenv("FUZZ_LEAKCHECK_FILE");
    chk = (int (*)(void))dlsym(RTLD_DEFAULT, "__lsan_do_recoverable_leak_check");
    if (!f || !chk) return;
    snprintf(path, sizeof path, "%s", f);
    unsetenv("LD_PRELOAD"); unsetenv("FUZZ_LEAKCHECK_FILE");
    pthread_t t; pthread_create(&t, 0, watch, 0); pthread_detach(t);
}
