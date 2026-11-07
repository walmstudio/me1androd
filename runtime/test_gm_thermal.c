/* Exercise GEARMASS thermal polling without Android hardware. The Android API
 * functions below are fakes; this is not a test of a real phone or driver. */
#ifndef __ANDROID__
#define __ANDROID__ 1
#endif
#include "gearmass.c"
#include <math.h>
int hle_heavy_tid(int n) { (void)n; return 0; }
uint64_t hle_heavy_wait_ns(int n) { (void)n; return 0; }
void hle_cpu_replace(int n) { (void)n; }
void hle_cpu_uclamp(int n) { (void)n; }
void hle_cpu_place(int n) { (void)n; }
uint64_t hle_fs_read_bytes(void) { return 0; }
int hle_cpu_avail(void) { return 0; }
int hle_fs_video_open(void) { return 0; }
const char* hle_fs_docs(void) { return NULL; }
static int ok, fail, status_value, calls;
static float headroom_value;
static int fake_status(void* m) { (void)m; return status_value; }
static float fake_headroom(void* m, int t) { (void)m; (void)t; calls++; return headroom_value; }
#define CHECK(x,desc) do { if (x) ok++; else { fail++; fprintf(stderr,"FAIL %d: %s\n", __LINE__,desc); } } while (0)
int main(void) {
    double h = -1; int st = -1;
    G.th_src = 1; AT.mgr = &G; AT.status = fake_status; AT.headroom = fake_headroom;
    status_value = 1; headroom_value = 0.87f;
    thermal_read(1, &h, &st);
    CHECK(h < 0 && st == 1 && calls == 0, "odd tick reads status but not headroom");
    thermal_read(2, &h, &st);
    CHECK(h > 0.86 && h < 0.88 && st == 1 && calls == 1, "valid Android readings");
    status_value = -1; headroom_value = NAN;
    thermal_read(3, &h, &st);
    CHECK(h > 0.86 && st == -1 && calls == 1, "between samples keep recent headroom only");
    thermal_read(4, &h, &st);
    CHECK(h < 0 && st == -1 && calls == 2, "invalid Android readings invalidate old values");
    headroom_value = 1.1f; status_value = 5;
    thermal_read(6, &h, &st);
    CHECK(h > 1 && st == 5, "Android readings recover");
    status_value = 99; headroom_value = -0.2f; thermal_read(8, &h, &st);
    CHECK(h < 0 && st == -1, "out of range readings are unknown");
    char path[] = "/tmp/me1-gm-temp-XXXXXX"; int fd = mkstemp(path);
    CHECK(fd >= 0, "create fake Linux thermal sensor");
    if (fd < 0) return 1;
    CHECK(write(fd, "45000\n", 6) == 6, "write fake thermal value"); close(fd);
    G.th_src = 3; TZ.n = 1; snprintf(TZ.temp[0], sizeof TZ.temp[0], "%s", path); TZ.trip[0] = 90000;
    thermal_read(1, &h, &st);
    CHECK(h == 0.5 && st == -1, "valid Linux sensor");
    unlink(path); thermal_read(2, &h, &st);
    CHECK(h < 0 && st == GM_THERMAL_PARTIAL, "disappeared Linux sensor invalidates old data");
    char hot[] = "/tmp/me1-gm-hot-XXXXXX", cool[] = "/tmp/me1-gm-cool-XXXXXX";
    int fh = mkstemp(hot), fc = mkstemp(cool);
    CHECK(fh >= 0 && fc >= 0, "create two sensor files");
    if (fh < 0 || fc < 0) return 1;
    CHECK(write(fh, "90000\n", 6) == 6 && write(fc, "45000\n", 6) == 6, "write sensor values");
    close(fh); close(fc);
    G.th_src=3; TZ.n=2; snprintf(TZ.temp[0],sizeof TZ.temp[0],"%s",hot); snprintf(TZ.temp[1],sizeof TZ.temp[1],"%s",cool);
    TZ.trip[0]=TZ.trip[1]=90000; thermal_read(1,&h,&st);
    CHECK(h == 1.0 && st == -1, "two complete sensors use hottest");
    { FILE *f=fopen(hot,"w"); if (!f) return 1; fputs("180000\n",f); fclose(f); }
    thermal_read(2,&h,&st);
    CHECK(h == 0.5 && st == GM_THERMAL_PARTIAL, "out-of-range sensor is incomplete");
    GmSample part = {0}; part.headroom = h; part.thermal_status = st;
    CHECK(!gm_heat_sample_complete(&part), "partial sample excluded from complete heat coverage");
    G.s = part; G.mode=GM_AUTO; G.cls=GMC_CPU; G.which=0; G.ncpu=8;
    char hud[160]; make_hud(); gm_hud(hud,sizeof hud);
    CHECK(strstr(hud,"heat >=50%?") != NULL, "HUD marks partial heat as an uncertain lower bound");
    part.headroom=-1; G.s=part; make_hud(); gm_hud(hud,sizeof hud);
    CHECK(strstr(hud,"heat ?") != NULL, "HUD marks entirely missing registered sensors");
    part.headroom=h;
    part.thermal_status = -1;
    CHECK(gm_heat_sample_complete(&part), "complete sample counts toward heat coverage");
    unlink(hot); thermal_read(2,&h,&st);
    CHECK(h == 0.5 && st == GM_THERMAL_PARTIAL, "missing hot sensor makes cool remainder partial");
    unlink(cool); thermal_read(3,&h,&st);
    CHECK(h < 0 && st == GM_THERMAL_PARTIAL, "all registered sensors missing are partial");
    G.th_src = 0; h = 0.5; st = 0; thermal_read(3, &h, &st);
    CHECK(h < 0 && st == -1, "no thermal source is unknown");
    printf("test_gm_thermal: %d ok, %d failed\n", ok, fail);
    return fail ? 1 : 0;
}
