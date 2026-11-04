// The newer SDL (Linux, with SDL3 built in: the Makefile's SDL3; the game and the updater).
//
// The programs carry their own SDL for the distributions that have none (Ubuntu 24.04 / Linux Mint 22), but a
// distribution's own can be newer and know more: controllers that came out after ours, fixes. Before anything uses
// SDL this opens the system's libSDL3.so.0, and when it is the same version as ours or newer, ours hands every call
// over to it (SDL's own dynamic API, SDL3_DYNAMIC_API), so a player on Arch runs Arch's SDL as before 0.11.4. No
// system SDL3, or an older one: ours. MHGU_SDL=bundled keeps ours; an SDL3_DYNAMIC_API the player set is left alone.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL_version.h>

char g_sdl_choice[256] = "built in";  // (for the log: runtime/ui.c)

#define VER(v) SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v)

// (102: after main.c's guest window, before any other constructor could use SDL)
__attribute__((constructor(102))) static void pick_sdl(void) {
    const char *own = getenv("SDL3_DYNAMIC_API"), *force = getenv("MHGU_SDL");
    if (own) { snprintf(g_sdl_choice, sizeof g_sdl_choice, "SDL3_DYNAMIC_API=%s", own); return; }
    if (force && !strcmp(force, "bundled")) { snprintf(g_sdl_choice, sizeof g_sdl_choice, "built in (MHGU_SDL)"); return; }
    void *lib = dlopen("libSDL3.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!lib) { snprintf(g_sdl_choice, sizeof g_sdl_choice, "built in (no system SDL3)"); return; }
    int (*get_version)(void) = (int (*)(void))dlsym(lib, "SDL_GetVersion");
    int v = get_version ? get_version() : 0;
    Dl_info info = {0};
    char path[200] = "libSDL3.so.0";
    if (get_version && dladdr((void *)get_version, &info) && info.dli_fname) snprintf(path, sizeof path, "%s", info.dli_fname);
    dlclose(lib);
    if (v >= SDL_VERSION) {
        setenv("SDL3_DYNAMIC_API", "libSDL3.so.0", 1);
        snprintf(g_sdl_choice, sizeof g_sdl_choice, "the system's (%s; built in: %d.%d.%d)", path, VER(SDL_VERSION));
    } else {
        snprintf(g_sdl_choice, sizeof g_sdl_choice, "built in (the system's %d.%d.%d is older)", VER(v));
    }
}
