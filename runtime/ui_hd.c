// Hot-swappable HD UI (ui_hd.h). The extraction tool writes the stock texel payloads and a
// manifest of (HD size/hash, stock size/hash); the HD payloads are already in the merged arcs.
// A texture is identified by the same FNV-1a the renderer computes for its storage.  When the
// variant size differs, the stock payload is uploaded into a host-owned guest buffer and the
// texture's dimensions/layout are scaled down (the pp4 pack is 2x or 4x in each dimension); the
// backend image is destroyed and recreated from the new layout on the next draw.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "nvn/nvn.h"
#include "rt.h"
#include "ui_hd.h"

typedef struct {
    uint32_t id, hd_size, sd_size;
    uint64_t hd_hash, sd_hash;
    int icon;                     // 1..8: a controller-glyph atlas (see ui_icons.c), 0: none
    uint8_t *sd;                  // stock payload, read on first use
} UiTex;

static UiTex *g_ui;
static int g_nui, g_uicap;
static int g_manifest;            // 0: not read, 1: ready, -1: unavailable
static uint64_t g_next_scan;

static bool manifest(void)
{
    if (g_manifest) return g_manifest > 0;
    char path[1200];
    snprintf(path, sizeof path, "%s/pp4_ui/ui.txt", g_game_dir);
    FILE *f = fopen(path, "rb");
    if (!f) {
        g_manifest = -1;
        LOG("[hd_ui] %s not found: the HD UI row is hidden (build with addons/pp4)\n", path);
        return false;
    }
    unsigned id, hd_size, sd_size, icon;
    unsigned long long hd_hash, sd_hash;
    while (fscanf(f, "%u %u %llx %u %llx %u", &id, &hd_size, &hd_hash, &sd_size, &sd_hash, &icon) == 6) {
        if (g_nui == g_uicap) {
            int n = g_uicap ? g_uicap * 2 : 256;
            UiTex *p = realloc(g_ui, (size_t)n * sizeof *p);
            if (!p) break;
            g_ui = p;
            g_uicap = n;
        }
        UiTex *e = &g_ui[g_nui++];
        memset(e, 0, sizeof *e);
        e->id = id;
        e->hd_size = hd_size;
        e->sd_size = sd_size;
        e->hd_hash = (uint64_t)hd_hash;
        e->sd_hash = (uint64_t)sd_hash;
        e->icon = (int)icon;
    }
    fclose(f);
    g_manifest = 1;
    LOG("[hd_ui] %d HD UI textures in %s\n", g_nui, path);
    return true;
}

bool hd_ui_available(void) { return manifest(); }

void hd_ui_init(void)
{
    if (manifest())
        LOG("[hd_ui] %d HD UI textures, HD UI %s\n", g_nui, g_config.hd_ui ? "On" : "Off");
}

static uint8_t *sd_payload(UiTex *e)
{
    if (e->sd) return e->sd;
    if (!e->sd_size) return NULL;
    char path[1200];
    snprintf(path, sizeof path, "%s/pp4_ui/%u.bin", g_game_dir, e->id);
    FILE *f = fopen(path, "rb");
    if (!f) { LOG("[hd_ui] cannot read %s\n", path); return NULL; }
    uint8_t *p = malloc(e->sd_size);
    if (!p || fread(p, 1, e->sd_size, f) != e->sd_size) {
        free(p);
        fclose(f);
        LOG("[hd_ui] cannot read %s\n", path);
        return NULL;
    }
    fclose(f);
    e->sd = p;
    return p;
}

static UiTex *find_hash(uint64_t total, uint64_t hash)
{
    for (int i = 0; i < g_nui; i++)
        if ((g_ui[i].hd_size == total && g_ui[i].hd_hash == hash) ||
            (g_ui[i].sd_size == total && g_ui[i].sd_hash == hash))
            return &g_ui[i];
    return NULL;
}

static UiTex *find_icon(int icon)
{
    if (icon <= 0) return NULL;
    for (int i = 0; i < g_nui; i++)
        if (g_ui[i].icon == icon) return &g_ui[i];
    return NULL;
}

static bool size_known(uint64_t total)
{
    for (int i = 0; i < g_nui; i++)
        if (g_ui[i].hd_size == total || g_ui[i].sd_size == total) return true;
    return false;
}

static uint64_t fnv(const uint8_t *p, uint64_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (uint64_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

void hd_ui_identify(NvnTexture *t, uint64_t total, uint64_t hash)
{
    if (!t || !manifest()) return;
    if (t->hd_ui_id > 0) {
        // A glyph set may have changed the hash of an atlas; t->icon still names it.
        if (t->icon > 0) {
            UiTex *e = find_icon(t->icon);
            if (e) {
                t->hd_ui_id = (int)e->id;
                t->hd_ui_variant = total == e->hd_size ? 1 : 0;
                t->hd_ui_hash = hash;
            }
        }
        return;
    }
    if (t->hd_ui_id < 0) return;
    UiTex *e = find_icon(t->icon);
    if (!e) e = find_hash(total, hash);
    if (!e) {
        if (size_known(total)) {
            if (++t->hd_ui_tries >= 4) t->hd_ui_id = -1;
        } else {
            t->hd_ui_id = -1;
        }
        return;
    }
    t->hd_ui_id = (int)e->id;
    t->hd_ui_hash = hash;
    t->hd_ui_variant = (total == e->hd_size && hash == e->hd_hash) ? 1 : 0;
    if (t->icon > 0 && e->icon == t->icon) t->hd_ui_variant = total == e->hd_size ? 1 : 0;
}

// The stock layout for a texture, found by scaling the live dimensions by k and trying the mip
// counts that make tegra_total_size() match the payload.  k is 1 (same size), 2 or 4 for the
// pp4 pack (a few 2x-area tutorial maps have no integer k and are left alone).
static bool fit_layout(NvnTexture *t, uint64_t size, TexLayout *out)
{
    TexLayout base = t->hd_ui_init_layout;
    for (uint32_t k = 1; k <= 8; k <<= 1) {
        if (t->hd_ui_init_width % k || t->hd_ui_init_height % k) continue;
        for (uint32_t lv = 1; lv <= base.levels; lv++) {
            TexLayout c = base;
            c.width = t->hd_ui_init_width / k;
            c.height = t->hd_ui_init_height / k;
            c.levels = lv;
            if (!c.width || !c.height) continue;
            tegra_layout_init(&c);
            if (tegra_total_size(&c) == size) {
                *out = c;
                return true;
            }
        }
    }
    return false;
}

static void save_init(NvnTexture *t)
{
    if (t->hd_ui_saved) return;
    t->hd_ui_saved = 1;
    t->hd_ui_init_addr = t->addr;
    t->hd_ui_init_layout = t->layout;
    t->hd_ui_init_width = t->width;
    t->hd_ui_init_height = t->height;
    t->hd_ui_init_levels = t->levels;
    t->hd_ui_init_stride = t->stride;
    t->hd_ui_init_flags = t->flags;
    t->hd_ui_init_total = tegra_total_size(&t->layout);
}

static bool apply_stock(NvnTexture *t, UiTex *e)
{
    uint8_t *p = sd_payload(e);
    if (!p) return false;
    save_init(t);
    TexLayout l;
    if (!fit_layout(t, e->sd_size, &l)) {
        static uint32_t warned;
        if (warned++ < 8)
            LOG("[hd_ui] texture %u: no stock layout for %u bytes (hd %ux%u), left HD\n", e->id, e->sd_size,
                t->hd_ui_init_width, t->hd_ui_init_height);
        t->hd_ui_id = -1;             // (a non-integer scale: this one stays HD in both states)
        return false;
    }
    if (!t->hd_ui_buf || t->hd_ui_buf_size != e->sd_size) {
        t->hd_ui_buf = hle_alloc(e->sd_size);
        if (!t->hd_ui_buf) return false;
        t->hd_ui_buf_size = e->sd_size;
    }
    memcpy(G2H(t->hd_ui_buf), p, e->sd_size);
    t->layout = l;
    t->width = l.width;
    t->height = l.height;
    t->levels = l.levels;
    t->stride = (t->hd_ui_init_flags & 0x10) ? t->hd_ui_init_stride / (t->hd_ui_init_width / l.width) : 0;
    t->addr = t->hd_ui_buf;
    t->hd_ui_variant = 0;
    t->hd_ui_hash = e->sd_hash;
    backend_texture_destroyed(t);
    return true;
}

static bool apply_hd(NvnTexture *t)
{
    if (!t->hd_ui_saved) return false;
    t->layout = t->hd_ui_init_layout;
    t->width = t->hd_ui_init_width;
    t->height = t->hd_ui_init_height;
    t->levels = t->hd_ui_init_levels;
    t->stride = t->hd_ui_init_stride;
    t->flags = t->hd_ui_init_flags;
    t->addr = t->hd_ui_init_addr;
    t->hd_ui_variant = 1;
    t->hd_ui_hash = 0;             // the glyph set may be in the storage; the icon scan knows
    backend_texture_destroyed(t);
    return true;
}

typedef struct { int changed; } Scan;

static void scan_texture(NvnTexture *t, void *arg)
{
    Scan *sc = arg;
    if (!t || !t->addr || t->target == 10) return;
    uint64_t total = tegra_total_size(&t->layout);
    if (t->hd_ui_id == 0) {
        if (t->icon > 0) {
            UiTex *e = find_icon(t->icon);
            if (e) {
                t->hd_ui_id = (int)e->id;
                t->hd_ui_variant = total == e->hd_size ? 1 : 0;
            }
        }
        if (t->hd_ui_id == 0) {
            if (!size_known(total)) { t->hd_ui_id = -1; return; }
            uint64_t h = fnv((const uint8_t *)G2H((uint32_t)t->addr), total);
            hd_ui_identify(t, total, h);
        }
    }
    if (t->hd_ui_id <= 0) return;
    UiTex *e = &g_ui[t->hd_ui_id - 1];
    int target = g_config.hd_ui ? 1 : 0;
    if (t->hd_ui_variant == target) return;
    bool ok = target ? apply_hd(t) : apply_stock(t, e);
    if (ok) sc->changed++;
}

void hd_ui_tick(void)
{
    if (!manifest()) return;
    uint64_t now = plat_now_ns();
    if (now < g_next_scan) return;
    g_next_scan = now + 1000000000ull;
    Scan sc = {0};
    nvn_textures_visit(scan_texture, &sc);
    if (sc.changed) LOG("[hd_ui] %s: %d textures\n", g_config.hd_ui ? "On" : "Off", sc.changed);
}

void hd_ui_set(int on)
{
    g_config.hd_ui = on ? 1 : 0;
    config_save_key("HDUi", on ? "On" : "Off");
    if (!manifest()) return;
    g_next_scan = 0;
    hd_ui_tick();
    LOG("[hd_ui] HD UI %s\n", on ? "On" : "Off");
}
