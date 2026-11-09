// First-launch display detection (graphics.ini `autodetect = 1`, shipped that way).
// Reads the main display from DXGI: resolution, refresh rate, Windows HDR on/off and peak nits.
// Turns HDR on when Windows HDR is on, sets HDR_PEAK to the display's peak (rounded to a menu
// choice), then writes those keys plus `autodetect = 0` back to graphics.ini so it runs once.
// `resolution = native` and the refresh rate are already read live every launch, so they are only logged.
#define COBJMACROS
#include <windows.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shim.h"

const char *ravengfx_get(const char *key);
void ravengfx_set(const char *key, const char *val);

// Replace `key = value` lines in the ini (or append them), keeping everything else.
static void ini_write(const char *path, const char **keys, const char **vals, int n) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    size_t cap = 1 << 16, len = 0;
    char *out = malloc(cap), line[512];
    int *done = calloc(n, sizeof *done);
    while (fgets(line, sizeof line, f)) {
        const char *s = line; while (*s == ' ' || *s == '\t') s++;
        int hit = -1;
        for (int i = 0; i < n && hit < 0; i++) {
            size_t kl = strlen(keys[i]);
            if (!strncmp(s, keys[i], kl) && (s[kl] == ' ' || s[kl] == '\t' || s[kl] == '=')) hit = i;
        }
        char tmp[512];
        if (hit >= 0) { snprintf(tmp, sizeof tmp, "%s = %s\n", keys[hit], vals[hit]); done[hit] = 1; }
        else snprintf(tmp, sizeof tmp, "%s", line);
        size_t tl = strlen(tmp);
        if (len + tl + 1 > cap) out = realloc(out, cap *= 2);
        memcpy(out + len, tmp, tl); len += tl;
    }
    fclose(f);
    for (int i = 0; i < n; i++) if (!done[i]) {
        char tmp[256]; snprintf(tmp, sizeof tmp, "%s = %s\n", keys[i], vals[i]);
        size_t tl = strlen(tmp);
        if (len + tl + 1 > cap) out = realloc(out, cap *= 2);
        memcpy(out + len, tmp, tl); len += tl;
    }
    if ((f = fopen(path, "w"))) { fwrite(out, 1, len, f); fclose(f); }
    free(out); free(done);
}

void display_detect(const char *ini_path) {
    const char *a = ravengfx_get("autodetect");
    if (!a || atoi(a) != 1) return;
    IDXGIFactory1 *fac = NULL;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&fac)) || !fac) { raven_log("[detect] no DXGI\n"); return; }
    int found = 0, hdr = 0; float peak = 0, ffpeak = 0; long w = 0, h = 0;
    IDXGIAdapter1 *ad;
    for (UINT ai = 0; !found && IDXGIFactory1_EnumAdapters1(fac, ai, &ad) != DXGI_ERROR_NOT_FOUND; ai++) {
        IDXGIOutput *out;
        for (UINT oi = 0; !found && IDXGIAdapter1_EnumOutputs(ad, oi, &out) != DXGI_ERROR_NOT_FOUND; oi++) {
            IDXGIOutput6 *o6 = NULL; DXGI_OUTPUT_DESC1 d;
            if (SUCCEEDED(IDXGIOutput_QueryInterface(out, &IID_IDXGIOutput6, (void **)&o6)) && o6 &&
                SUCCEEDED(IDXGIOutput6_GetDesc1(o6, &d)) && d.AttachedToDesktop &&
                d.DesktopCoordinates.left == 0 && d.DesktopCoordinates.top == 0) {   // primary display
                found = 1;
                hdr = d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                peak = d.MaxLuminance; ffpeak = d.MaxFullFrameLuminance;
                w = d.DesktopCoordinates.right; h = d.DesktopCoordinates.bottom;
            }
            if (o6) IDXGIOutput6_Release(o6);
            IDXGIOutput_Release(out);
        }
        IDXGIAdapter1_Release(ad);
    }
    IDXGIFactory1_Release(fac);
    DEVMODEA dm = {0}; dm.dmSize = sizeof dm;
    unsigned hz = EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm) ? dm.dmDisplayFrequency : 0;
    if (!found) { raven_log("[detect] main display not found, keeping settings\n"); return; }

    // HDR_PEAK menu choices (ravenui_win.c): pick the nearest to the display's reported peak.
    static const int choices[] = {400, 600, 800, 1000, 1500, 2000, 4000};
    int best = 1000;
    if (peak > 100) { best = choices[0];
        for (unsigned i = 0; i < sizeof choices / sizeof *choices; i++)
            if (abs(choices[i] - (int)peak) < abs(best - (int)peak)) best = choices[i]; }
    char sp[16]; snprintf(sp, sizeof sp, "%d", best);
    const char *keys[] = {"HDR", "HDR_PEAK", "autodetect"}, *vals[] = {hdr ? "1" : "0", sp, "0"};
    for (int i = 0; i < 3; i++) ravengfx_set(keys[i], vals[i]);
    ini_write(ini_path, keys, vals, 3);
    raven_log("[detect] main display %ldx%ld (aspect %.3f) %u Hz, Windows HDR %s, peak %.0f nits (full-frame %.0f)"
              " -> HDR = %d, HDR_PEAK = %s, saved to %s\n", w, h, h ? (double)w / h : 0, hz,
              hdr ? "on" : "off", peak, ffpeak, hdr, sp, ini_path);
}
