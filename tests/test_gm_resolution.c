#define _GNU_SOURCE
#include "me1settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int ok, fail, mixed_field = 0;
static double heat_known = 100;
#define CHECK(x,desc) do { if(x) ok++; else { fail++; fprintf(stderr,"FAIL line %d: %s\n",__LINE__,desc); } } while(0)
static void sample(const char* dir, const char* id, const char* res, int play, double fps, int gpu, int cap, int heat) {
    char path[1024]; snprintf(path,sizeof path,"%s/me1_gearmass_last.txt",dir); FILE* f=fopen(path,"w");
    if(!f) { perror("sample"); exit(1); }
    fprintf(f,"run_id=%s\nmode=AUTO\nresolution=%s\nactive_seconds=%d\nactive_fps=%.1f\nfps_limit=30\nactive_gpu=%d\nactive_capped=%d\nheat_max=%d\n",id,res,play,fps,gpu,cap,heat);
    if (mixed_field >= 0) fprintf(f,"fps_limit_mixed=%d\n", mixed_field);
    if (heat_known >= 0) fprintf(f,"active_heat_known=%.1f\n", heat_known);
    fclose(f);
}
static int set(ME1Config* c,const char* key,const char* val) { char e[256]; int r=me1cfg_set(c,key,val,e,sizeof e); if(r) fprintf(stderr,"%s\n",e); return r; }
static const char* value(ME1Config* c,const char* id) { int i; return me1cfg_find(id,&i) ? c->value[i] : ""; }
int main(void) {
    char root[]="/tmp/me1resXXXXXX", msg[4096]; char* dir=mkdtemp(root); if(!dir) return 2;
    unsetenv("ME1_GEARMASS"); unsetenv("ME1_GM_RES_SKIP"); unsetenv("ME1_FPS_LIMIT");
    ME1Config c; me1cfg_load(&c,dir,dir); set(&c,"screen.resolution","1280x720"); set(&c,"port.fps_limit","30"); me1cfg_save(&c,msg,sizeof msg);
    sample(dir,"a","1280x720",300,20,60,5,70);
    CHECK(me1cfg_adapt_resolution(&c,msg,sizeof msg)==0 && !strcmp(value(&c,"screen.resolution"),"1280x720"),"default disabled");
    set(&c,"port.gm_autores","1"); me1cfg_save(&c,msg,sizeof msg);
    CHECK(me1cfg_adapt_resolution(&c,msg,sizeof msg)==1 && !strcmp(value(&c,"screen.resolution"),"1120x630"),"GPU-bound downscale");
    ME1Config reload; me1cfg_load(&reload,dir,dir);
    CHECK(!strcmp(value(&reload,"screen.resolution"),"1120x630"),"adjustment persists after reload");
    set(&c,"port.fps_limit","45"); set(&reload,"screen.resolution","960x540"); me1cfg_save(&reload,msg,sizeof msg);
    CHECK(!me1cfg_refresh_clean(&c) && !strcmp(value(&c,"port.fps_limit"),"45") && !strcmp(value(&c,"screen.resolution"),"960x540"), "refresh preserves unsaved changes and adopts saved resolution");
    set(&c,"port.fps_limit","30"); set(&c,"screen.resolution","1120x630"); me1cfg_save(&c,msg,sizeof msg);
    sample(dir,"a","1120x630",300,20,60,5,70);
    CHECK(me1cfg_adapt_resolution(&c,msg,sizeof msg)==0 && !strcmp(value(&c,"screen.resolution"),"1120x630"),"one run id consumed once");
    sample(dir,"b","1120x630",300,30,0,90,70);
    CHECK(me1cfg_adapt_resolution(&c,msg,sizeof msg)==0 && !strcmp(value(&c,"screen.resolution"),"1120x630"),"one good session waits");
    sample(dir,"b1","1120x630",300,30,0,90,70);
    CHECK(me1cfg_adapt_resolution(&c,msg,sizeof msg)==1 && !strcmp(value(&c,"screen.resolution"),"1280x720"),"two good sessions raise to ceiling");
    sample(dir,"c","1280x720",300,30,0,90,70);
    CHECK(me1cfg_adapt_resolution(&c,msg,sizeof msg)==0 && !strcmp(value(&c,"screen.resolution"),"1280x720"),"never above ceiling");
    sample(dir,"ca","1280x720",300,20,0,0,50);
    CHECK(me1cfg_adapt_resolution(&c,msg,sizeof msg)==0 && !strcmp(value(&c,"screen.resolution"),"1280x720"),"CPU or unknown GPU does not trigger downscale");
    sample(dir,"d","1280x720",179,20,60,5,70);
    CHECK(me1cfg_adapt_resolution(&c,msg,sizeof msg)==0,"short session ignored");
    sample(dir,"e","1024x576",300,20,60,5,70);
    CHECK(me1cfg_adapt_resolution(&c,msg,sizeof msg)==0,"different resolution ignored");
    sample(dir,"f","1280x720",300,20,60,5,70); setenv("ME1_GM_RES_SKIP","1",1);
    CHECK(me1cfg_adapt_resolution(&c,msg,sizeof msg)==0,"headless test skipped"); unsetenv("ME1_GM_RES_SKIP");
    setenv("ME1_GEARMASS","max",1); CHECK(me1cfg_adapt_resolution(&c,msg,sizeof msg)==0,"non-auto skipped"); unsetenv("ME1_GEARMASS");
    CHECK(me1cfg_adapt_resolution(&c,msg,sizeof msg)==1 && !strcmp(value(&c,"screen.resolution"),"1120x630"),"unconsumed sample still usable");
    sample(dir,"1a","1120x630",300,30,0,90,95);
    CHECK(me1cfg_adapt_resolution(&c,msg,sizeof msg)==0 && !strcmp(value(&c,"screen.resolution"),"1120x630"),"no upscaling while hot");
    set(&c,"screen.resolution","1920x1080"); me1cfg_save(&c,msg,sizeof msg);
    sample(dir,"1b","1920x1080",300,20,60,5,50);
    CHECK(me1cfg_adapt_resolution(&c,msg,sizeof msg)==1 && !strcmp(value(&c,"screen.resolution"),"1680x944"),"manual resolution resets ceiling");
    sample(dir,"1c","1680x944",300,30,0,90,50);
    CHECK(me1cfg_adapt_resolution(&c,msg,sizeof msg)==0,"one good session waits at new ceiling");
    sample(dir,"1c1","1680x944",300,30,0,90,50);
    CHECK(me1cfg_adapt_resolution(&c,msg,sizeof msg)==1 && !strcmp(value(&c,"screen.resolution"),"1920x1080"),"recover new ceiling");
    set(&c,"port.fps_limit","0"); sample(dir,"1d","1920x1080",300,20,60,5,50);
    CHECK(me1cfg_adapt_resolution(&c,msg,sizeof msg)==0,"different or missing cap ignored");
    set(&c,"port.gm_autores","0"); sample(dir,"1e","1920x1080",300,20,60,5,50);
    CHECK(me1cfg_adapt_resolution(&c,msg,sizeof msg)==0,"disabled after use");
    /* A mixed FPS cap and legacy records must not drive next-session changes. */
    set(&c,"port.gm_autores","1"); set(&c,"port.fps_limit","30"); set(&c,"screen.resolution","1280x720"); me1cfg_save(&c,msg,sizeof msg);
    mixed_field=1; sample(dir,"aa0","1280x720",300,20,60,5,50);
    CHECK(me1cfg_adapt_resolution(&c,msg,sizeof msg)==0 && !strcmp(value(&c,"screen.resolution"),"1280x720"),"mixed FPS cap is ignored");
    mixed_field=2; sample(dir,"aaf","1280x720",300,20,60,5,50);
    CHECK(me1cfg_adapt_resolution(&c,msg,sizeof msg)==0 && !strcmp(value(&c,"screen.resolution"),"1280x720"),"invalid cap consistency flag is ignored");
    mixed_field=-1; sample(dir,"aa1","1280x720",300,20,60,5,50);
    CHECK(me1cfg_adapt_resolution(&c,msg,sizeof msg)==0 && !strcmp(value(&c,"screen.resolution"),"1280x720"),"legacy record without cap consistency is ignored");
    mixed_field=0; sample(dir,"aa2","1280x720",300,20,60,5,50);
    CHECK(me1cfg_adapt_resolution(&c,msg,sizeof msg)==1 && !strcmp(value(&c,"screen.resolution"),"1120x630"),"stable FPS cap can downscale");
    sample(dir,"aa3","1120x630",300,30,0,90,-1);
    CHECK(me1cfg_adapt_resolution(&c,msg,sizeof msg)==0,"unknown heat first session cannot count for upscale");
    sample(dir,"aa4","1120x630",300,30,0,90,-1);
    CHECK(me1cfg_adapt_resolution(&c,msg,sizeof msg)==0 && !strcmp(value(&c,"screen.resolution"),"1120x630"),"unknown heat never authorizes upscale");
    heat_known=50;
    sample(dir,"aa5","1120x630",300,30,0,90,50);
    CHECK(me1cfg_adapt_resolution(&c,msg,sizeof msg)==0,"partial heat coverage first session cannot count");
    sample(dir,"aa6","1120x630",300,30,0,90,50);
    CHECK(me1cfg_adapt_resolution(&c,msg,sizeof msg)==0 && !strcmp(value(&c,"screen.resolution"),"1120x630"),"partial heat coverage never authorizes upscale");
    heat_known=-1; sample(dir,"aa7","1120x630",300,30,0,90,50);
    CHECK(me1cfg_adapt_resolution(&c,msg,sizeof msg)==0,"missing heat coverage is not evidence of cooling");
    heat_known=94.9;
    sample(dir,"aa7a","1120x630",300,30,0,90,50);
    CHECK(me1cfg_adapt_resolution(&c,msg,sizeof msg)==0,"94.9% thermal coverage first session cannot count");
    sample(dir,"aa7b","1120x630",300,30,0,90,50);
    CHECK(me1cfg_adapt_resolution(&c,msg,sizeof msg)==0 && !strcmp(value(&c,"screen.resolution"),"1120x630"),"94.9% thermal coverage cannot upscale");
    heat_known=95.0;
    sample(dir,"aa8","1120x630",300,30,0,90,50);
    CHECK(me1cfg_adapt_resolution(&c,msg,sizeof msg)==0,"first confirmed cool session waits");
    sample(dir,"aa9","1120x630",300,30,0,90,50);
    CHECK(me1cfg_adapt_resolution(&c,msg,sizeof msg)==1 && !strcmp(value(&c,"screen.resolution"),"1280x720"),"two cool sessions with 95% coverage permit upscale");
    char cmd[1200]; snprintf(cmd,sizeof cmd,"rm -rf '%s'",dir); system(cmd);
    printf("test_gm_resolution: %d ok, %d failed\n",ok,fail); return fail ? 1 : 0;
}
