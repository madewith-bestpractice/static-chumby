#include "common.h"
#include <limits.h>
#include <pthread.h>
#include <unistd.h>

static char dir[256];
static pthread_once_t once = PTHREAD_ONCE_INIT;

static void init(void) {
    char exe[256];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) { snprintf(dir, sizeof dir, "%s", STATIC_DIR); return; }
    exe[n] = 0;
    char *slash = strrchr(exe, '/');
    if (slash) *slash = 0;
    snprintf(dir, sizeof dir, "%s", exe[0] ? exe : "/");
}

const char *app_dir(void) { pthread_once(&once, init); return dir; }

int app_installed(void) { return strcmp(app_dir(), STATIC_DIR) == 0; }

/* A handful of names (cacert.pem, ffmpeg): interned so callers can keep the pointer. */
const char *app_file(const char *name) {
    static struct { char name[32]; char path[300]; } tab[8];
    static int n;
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    const char *r = NULL;
    for (int i = 0; i < n && !r; i++) if (!strcmp(tab[i].name, name)) r = tab[i].path;
    if (!r && n < 8) {
        snprintf(tab[n].name, sizeof tab[n].name, "%s", name);
        snprintf(tab[n].path, sizeof tab[n].path, "%s/%s", app_dir(), name);
        r = tab[n++].path;
    }
    pthread_mutex_unlock(&mu);
    return r ? r : name;
}
