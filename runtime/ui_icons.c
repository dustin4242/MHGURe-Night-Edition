// Hot-swappable controller glyphs (ui_icons.h). The five textures are the ones that differ between the
// Layout variants; their texel payloads are byte-identical to the game's texture storage, so swapping is a
// memcpy into the NVN texture's guest memory plus a backend dirty mark.
#include <stdio.h>
#include <strings.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "nvn/nvn.h"
#include "rt.h"
#include "ui_icons.h"
#include "ui_hd.h"

enum { ICON_N = 8 };
static const char *const g_ids[ICON_N] = {
    "HD_custom_pnl2", "HD_window", "HD_lobby00", "HD_hud00", "button_festa",
    "HD_custom_pnl2_loc", "HD_window_loc", "button_festa_loc",
};
static const char *const g_icon_names[3] = {"Switch", "Xbox", "Playstation"};

typedef struct { uint32_t size; uint64_t hash; uint8_t *data; } IconSet;
static IconSet g_set[3][ICON_N];
static int g_manifest;                        // 0: not read, 1: ready, -1: unavailable
static uint64_t g_next_scan;

static int id_index(const char *s)
{
    for (int i = 0; i < ICON_N; i++)
        if (!strcasecmp(s, g_ids[i])) return i;
    return -1;
}

static int icon_index(const char *s)
{
    for (int i = 0; i < 3; i++)
        if (!strcasecmp(s, g_icon_names[i])) return i;
    return -1;
}

const char *icons_name(int v) { return v >= 0 && v < 3 ? g_icon_names[v] : g_icon_names[0]; }

static bool manifest(void)
{
    if (g_manifest) return g_manifest > 0;
    char path[1200];
    snprintf(path, sizeof path, "%s/pp4_icons/icons.txt", g_game_dir);
    FILE *f = fopen(path, "rb");
    if (!f) {
        g_manifest = -1;
        LOG("[icons] %s not found: controller glyph swapping is off (build with addons/pp4)\n", path);
        return false;
    }
    char icon[32], id[64], hex[32];
    unsigned size;
    while (fscanf(f, "%31s %63s %u %31s", icon, id, &size, hex) == 4) {
        int v = icon_index(icon), i = id_index(id);
        if (v < 0 || i < 0) continue;
        g_set[v][i].size = size;
        g_set[v][i].hash = strtoull(hex, NULL, 16);
    }
    fclose(f);
    g_manifest = 1;
    return true;
}

static uint8_t *payload(int v, int i)
{
    IconSet *s = &g_set[v][i];
    if (s->data) return s->data;
    char path[1200];
    snprintf(path, sizeof path, "%s/pp4_icons/%s/%s.bin", g_game_dir, icons_name(v), g_ids[i]);
    FILE *f = fopen(path, "rb");
    if (!f) { LOG("[icons] cannot read %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n != (long)s->size) {
        LOG("[icons] %s: %ld bytes, expected %u\n", path, n, s->size);
        fclose(f);
        return NULL;
    }
    uint8_t *p = malloc(s->size ? s->size : 1);
    if (!p || fread(p, 1, s->size, f) != s->size) {
        free(p);
        fclose(f);
        LOG("[icons] cannot read %s\n", path);
        return NULL;
    }
    fclose(f);
    s->data = p;
    return p;
}

static uint64_t fnv(const uint8_t *p, uint64_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (uint64_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

bool icons_available(void) { return manifest(); }

void icons_init(void)
{
    if (!manifest()) return;
    for (int i = 0; i < ICON_N; i++) payload(g_config.icons, i);
    LOG("[icons] %s controller glyphs\n", icons_name(g_config.icons));
}

bool icons_texture(struct NvnTexture *t, const uint8_t *mem, uint64_t total, uint64_t *hash)
{
    (void)mem;
    if (!t->addr || !manifest()) return false;
    int id = t->icon;                          // 1-based, 0 = not identified yet, -1 = not a glyph
    if (id < 0) return false;
    if (id == 0) {
        for (int k = 0; k < ICON_N && id == 0; k++)
            for (int v = 0; v < 3; v++)
                if (g_set[v][k].size == total && g_set[v][k].hash == *hash) { id = k + 1; t->icon = id; break; }
        if (id == 0) return false;
    }
    int k = id - 1, sel = g_config.icons;
    if (sel < 0 || sel > 2 || !g_set[sel][k].size || g_set[sel][k].size != total) return false;
    if (*hash == g_set[sel][k].hash) return false;
    if (!g_config.hd_ui && hd_ui_available()) return false;  // HD UI off: identify, keep stock texels
    uint8_t *p = payload(sel, k);
    if (!p) return false;
    memcpy(G2H((uint32_t)t->addr), p, total);
    t->icon_hash = g_set[sel][k].hash;
    *hash = g_set[sel][k].hash;
    return true;
}

typedef struct { int sel, found[ICON_N], changed; } Scan;

static void scan_texture(struct NvnTexture *t, void *arg)
{
    Scan *sc = arg;
    if (t->icon < 0 || !t->addr) return;
    uint64_t total = tegra_total_size(&t->layout);
    int k = t->icon > 0 ? t->icon - 1 : -1;
    if (k >= 0 && g_set[sc->sel][k].size != total) return;
    if (k >= 0 && t->icon_hash == g_set[sc->sel][k].hash) { sc->found[k] = 1; return; }
    // Only the five glyph sizes can match; other textures of those sizes are hashed once.
    int size_ok = 0;
    for (int i = 0; i < ICON_N; i++) if (g_set[sc->sel][i].size == total) size_ok = 1;
    if (!size_ok) return;
    uint64_t h = fnv((const uint8_t *)G2H((uint32_t)t->addr), total);
    if (k < 0) {
        for (int i = 0; i < ICON_N && k < 0; i++)
            for (int v = 0; v < 3; v++)
                if (g_set[v][i].size == total && g_set[v][i].hash == h) { k = i; break; }
        if (k < 0) { if (++t->icon_tries >= 4) t->icon = -1; return; }
        t->icon = k + 1;
    }
    if (h == g_set[sc->sel][k].hash) { t->icon_hash = h; sc->found[k] = 1; return; }
    if (!g_config.hd_ui && hd_ui_available()) { t->icon_hash = h; sc->found[k] = 1; return; }  // stock
    uint8_t *p = payload(sc->sel, k);
    if (!p) return;
    memcpy(G2H((uint32_t)t->addr), p, total);
    t->icon_hash = g_set[sc->sel][k].hash;
    backend_texture_dirty(t);
    sc->found[k] = 1;
    sc->changed = 1;
}

void icons_tick(void)
{
    if (!manifest()) return;
    uint64_t now = plat_now_ns();
    if (now < g_next_scan) return;
    g_next_scan = now + 1000000000ull;
    Scan sc = {g_config.icons, {0}, 0};
    nvn_textures_visit(scan_texture, &sc);
    if (sc.changed) LOG("[icons] %s controller glyphs applied\n", icons_name(sc.sel));
}

void icons_set(int v)
{
    if (v < 0 || v > 2) return;
    g_config.icons = v;
    config_save_key("Icons", icons_name(v));
    if (!manifest()) return;
    g_next_scan = 0;
    icons_tick();
    LOG("[icons] %s controller glyphs\n", icons_name(v));
}
