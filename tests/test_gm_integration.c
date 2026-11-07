/* End-to-end synthetic GEARMASS session -> statistics file -> resolution policy.
 * The runtime and sensor data are simulated; this is not a device or game test. */
#define _GNU_SOURCE
#include "../runtime/gearmass.c"
#include "../settings/me1settings.h"
int hle_heavy_tid(int n) { (void)n; return 0; }
uint64_t hle_heavy_wait_ns(int n) { (void)n; return 0; }
void hle_cpu_replace(int n) { (void)n; }
void hle_cpu_uclamp(int n) { (void)n; }
void hle_cpu_place(int n) { (void)n; }
uint64_t hle_fs_read_bytes(void) { return 0; }
int hle_cpu_avail(void) { return 0; }
int hle_fs_video_open(void) { return 0; }
const char* hle_fs_docs(void) { return getenv("ME1_DOCS"); }
static int ok, fail;
#define CHECK(x, desc) do { if (x) ok++; else { fail++; fprintf(stderr, "FAIL %d: %s\n", __LINE__, desc); } } while (0)
static int set(ME1Config* c, const char* key, const char* value) { char e[256]; int r=me1cfg_set(c,key,value,e,sizeof e); if(r) fprintf(stderr,"%s\n",e); return r; }
static const char* value(ME1Config* c) { int i; return me1cfg_find("screen.resolution",&i) ? c->value[i] : ""; }
static void session(const char* id, int w, int h, double fps, double gpu, double capped, double heat, double known, int mixed, int no_frame) {
    char res[32]; snprintf(res,sizeof res,"%dx%d",w,h); setenv("ME1_GM_RENDER_RES",res,1);
    G.mode=GM_AUTO; G.ncpu=8; G.t_start=gm_now()-300; snprintf(G.run_id,sizeof G.run_id,"%s",id);
    G.st_play=G.st_active=300; G.st_fps=G.st_active_fps=fps*300;
    G.st_partial=0; G.st_active_gpu=gpu*300; G.st_active_cap=capped*300; G.st_active_heat_known=known*300; G.st_heat=heat;
    G.res_frame=0; G.res_mixed=0; G.limit_first=0; G.limit_mixed=0; G.limit=0;
    gm_frame_size((unsigned)w,(unsigned)h);
    if (!no_frame) { gm_frame(100,0,mixed==2 ? 0 : 30); if(mixed==1) { gm_frame(200,0,45); gm_frame(300,0,30); } else if(mixed==2) gm_frame(200,0,30); }
    gm_session_save();
}
int main(void) {
    char dir[]="/tmp/me1-gm-integr-XXXXXX", msg[4096], command[1024];
    if (!mkdtemp(dir)) return 2;
    setenv("ME1_DOCS",dir,1); unsetenv("ME1_GM_RES_SKIP"); unsetenv("ME1_GEARMASS"); unsetenv("ME1_FPS_LIMIT");
    ME1Config c; me1cfg_load(&c,dir,dir);
    CHECK(!set(&c,"screen.resolution","1280x720") && !set(&c,"port.fps_limit","30") && !set(&c,"port.gm_autores","1"), "initial settings");
    CHECK(!me1cfg_save(&c,msg,sizeof msg), "save initial settings");
    session("100",1280,720,20,0.6,0.05,0.5,1.0,0,0);
    CHECK(me1cfg_adapt_resolution(&c,msg,sizeof msg)==1 && !strcmp(value(&c),"1120x630"), "real report triggers downscale");
    session("101",1120,630,30,0,0.9,0.5,0.5,0,0);
    CHECK(me1cfg_adapt_resolution(&c,msg,sizeof msg)==0, "50% coverage first session");
    session("102",1120,630,30,0,0.9,0.5,0.5,0,0);
    CHECK(me1cfg_adapt_resolution(&c,msg,sizeof msg)==0 && !strcmp(value(&c),"1120x630"), "50% coverage cannot upscale");
    session("102a",1120,630,30,0,0.9,0.5,0.9499,0,0);
    CHECK(me1cfg_adapt_resolution(&c,msg,sizeof msg)==0, "94.99% coverage first session");
    session("102b",1120,630,30,0,0.9,0.5,0.9499,0,0);
    CHECK(me1cfg_adapt_resolution(&c,msg,sizeof msg)==0 && !strcmp(value(&c),"1120x630"), "coverage truncation does not round 94.99% up to 95%");
    session("103",1120,630,30,0,0.9,0.5,0.95,0,0);
    CHECK(me1cfg_adapt_resolution(&c,msg,sizeof msg)==0, "first cool session waits");
    session("104",1120,630,30,0,0.9,0.5,0.95,0,0);
    CHECK(me1cfg_adapt_resolution(&c,msg,sizeof msg)==1 && !strcmp(value(&c),"1280x720"), "two full-coverage cool sessions upscale");
    session("105",1280,720,20,0.6,0.05,0.5,1.0,1,0);
    CHECK(me1cfg_adapt_resolution(&c,msg,sizeof msg)==0 && !strcmp(value(&c),"1280x720"), "mixed limits, including changed back, ignored");
    session("105a",1280,720,20,0.6,0.05,0.5,1.0,2,0);
    CHECK(me1cfg_adapt_resolution(&c,msg,sizeof msg)==0 && !strcmp(value(&c),"1280x720"), "uncapped then capped is mixed");
    session("106",1280,720,20,0.6,0.05,0.5,1.0,0,1);
    CHECK(me1cfg_adapt_resolution(&c,msg,sizeof msg)==0, "no observed frames cannot claim stable limit");
    session("107",1280,720,20,0.6,0.05,0.5,1.0,0,0);
    CHECK(me1cfg_adapt_resolution(&c,msg,sizeof msg)==1 && !strcmp(value(&c),"1120x630"), "stable next session can downscale again");
    session("108",1120,630,30,0,0.9,-1,0,0,0);
    CHECK(me1cfg_adapt_resolution(&c,msg,sizeof msg)==0, "unknown heat first session");
    session("109",1120,630,30,0,0.9,-1,0,0,0);
    CHECK(me1cfg_adapt_resolution(&c,msg,sizeof msg)==0 && !strcmp(value(&c),"1120x630"), "unknown heat cannot upscale");
    /* Simulate samples through the runtime accounting path, write the actual
     * report and feed it to the resolution policy. */
    GmSample heat_sample={0}; heat_sample.headroom=0.5; heat_sample.thermal_status=GM_THERMAL_PARTIAL;
    session("110",1120,630,30,0,0.9,0.5,0,0,0);
    for(int i=0;i<300;i++) { gm_record_active_heat(&heat_sample,1); gm_record_thermal(&heat_sample,1); }
    gm_session_save();
    { char path[1024], line[256]; double seconds=-1; snprintf(path,sizeof path,"%s/me1_gearmass_last.txt",dir);
      FILE* f=fopen(path,"r"); if(f) { while(fgets(line,sizeof line,f)) if(sscanf(line,"thermal_partial_seconds=%lf",&seconds)==1) break; fclose(f); }
      CHECK(seconds==300, "partial telemetry duration appears in the real session report"); }
    CHECK(me1cfg_adapt_resolution(&c,msg,sizeof msg)==0 && !strcmp(value(&c),"1120x630"), "first actual partial-heat session cannot upscale");
    session("111",1120,630,30,0,0.9,0.5,0,0,0);
    for(int i=0;i<300;i++) { gm_record_active_heat(&heat_sample,1); gm_record_thermal(&heat_sample,1); }
    gm_session_save();
    CHECK(me1cfg_adapt_resolution(&c,msg,sizeof msg)==0 && !strcmp(value(&c),"1120x630"), "two actual partial-heat sessions cannot upscale");
    heat_sample.thermal_status=-1;
    session("112",1120,630,30,0,0.9,0.5,0,0,0);
    for(int i=0;i<300;i++) { gm_record_active_heat(&heat_sample,1); gm_record_thermal(&heat_sample,1); }
    gm_session_save();
    CHECK(me1cfg_adapt_resolution(&c,msg,sizeof msg)==0, "first complete cool session waits");
    session("113",1120,630,30,0,0.9,0.5,0,0,0);
    for(int i=0;i<300;i++) { gm_record_active_heat(&heat_sample,1); gm_record_thermal(&heat_sample,1); }
    gm_session_save();
    CHECK(me1cfg_adapt_resolution(&c,msg,sizeof msg)==1 && !strcmp(value(&c),"1280x720"), "two actual complete cool sessions can upscale");
    snprintf(command,sizeof command,"rm -rf '%s'",dir); system(command);
    printf("test_gm_integration: %d ok, %d failed\n",ok,fail); return fail ? 1 : 0;
}
