/* Synthetic loss is injected in Present or submit; it does not simulate an actual driver crash. */
#define main test_gl_original_main
#include "test_gl.c"
#undef main
#include <unistd.h>
#define LOST 0x88760868u
#define NOTRESET 0x88760869u
static void bind_test_state(uint32_t vs, uint32_t ps, uint32_t vd, uint32_t t, uint32_t vb) {
    uint32_t c0 = gbuf(16);
    com(dev, 87, 1, vd); com(dev, 92, 1, vs); com(dev, 94, 3, 0u, c0, 1u); com(dev, 107, 1, ps);
    com(dev, 65, 2, 0u, t); com(dev, 100, 4, 0u, vb, 0u, 24u); rs(22, 1); rs(7, 0); point(0);
}
static void render_check(uint32_t bb, uint32_t query, const char* label) {
    uint32_t qr=gbuf(4), hr=1; int spins=0;
    CHECK(com(dev,41,0)==0, "BeginScene"); clear(0xFF000000u);
    CHECK(com(query,6,1,2u)==0, "Issue(BEGIN)");
    CHECK(com(dev,81,3,5u,0u,2u)==0, "DrawPrimitive");
    CHECK(com(query,6,1,1u)==0, "Issue(END)");
    while ((hr=com(query,7,3,qr,4u,1u))==1 && spins++<100000) {}
    CHECK(hr==0 && rd32(qr)==4096, "%s query result %u hr %x",label,rd32(qr),hr);
    grab(bb); quads(0x23C778,0x23C778,0x23C778,0x23C778,label);
    CHECK(com(dev,42,0)==0, "EndScene");
}
int main(int argc, char** argv) {
    int timeout = argc > 1 && !strcmp(argv[1], "timeout");
    rt_init(); out=gbuf(64); vtx=gbuf(96);
    uint32_t d3d=api("d3d9.dll","Direct3DCreate9",1,32u), pp=gbuf(56);
    wr32(pp,64); wr32(pp+4,64); wr32(pp+8,22); wr32(pp+12,1); wr32(pp+24,1); wr32(pp+28,0x10000); wr32(pp+32,1); wr32(pp+36,1); wr32(pp+40,75);
    CHECK(com(d3d,16,6,0u,1u,0x10000u,0x40u,pp,out)==0 && rd32(out), "CreateDevice"); dev=rd32(out);
    CHECK(com(dev,18,4,0u,0u,0u,out)==0,"GetBackBuffer"); uint32_t bb=rd32(out);
    CHECK(com(dev,36,6,64u,64u,21u,2u,out,0u)==0,"Create SYSTEMMEM"); sysrb=rd32(out);
    static const uint32_t vst[]={0xFFFE0300u,DCL(U_POS,0,R_IN,0),DCL(U_TC,0,R_IN,1),DCL(U_COLOR,0,R_IN,2),DCL(U_POS,0,R_OUT,0),DCL(U_TC,0,R_OUT,1),DCL(U_COLOR,0,R_OUT,2),ADD(DST(R_OUT,0),SRC(R_IN,0),SRC(R_CONST,0)),MOV(DST(R_OUT,1),SRC(R_IN,1)),MOV(DST(R_OUT,2),SRC(R_IN,2)),END};
    static const uint32_t pst[]={0xFFFF0300u,DCL(U_TC,0,R_IN,0),DCL(U_COLOR,0,R_IN,1),DCL2D(0),TEXLD(DST(R_TEMP,0),SRC(R_IN,0),SRC(R_SAMP,0)),MUL(DST(R_COLOROUT,0),SRC(R_TEMP,0),SRC(R_IN,1)),END};
    uint32_t vs=shader(91,vst,sizeof vst/4), ps=shader(106,pst,sizeof pst/4), de=gbuf(32);
    static const uint8_t decl[32]={0,0,0,0,2,0,0,0,0,0,12,0,1,0,5,0,0,0,20,0,4,0,10,0,0xFF,0,0,0,17,0,0,0};
    memcpy(M+de,decl,32); CHECK(com(dev,86,2,de,out)==0,"CreateVertexDeclaration"); uint32_t vd=rd32(out);
    uint32_t t=tex(1,1,1,0,21,1), color=0xFF23C778u; fill(t,0,&color,4,1);
    CHECK(com(dev,26,6,96u,0u,0u,1u,out,0u)==0,"Create managed VB"); uint32_t vb=rd32(out), lr=gbuf(4);
    quad_at(vtx,-1,1,1,-1,.5f,0xFFFFFFFFu);
    CHECK(com(vb,11,4,0u,0u,lr,0u)==0,"Lock VB"); memcpy(M+rd32(lr),M+vtx,96); CHECK(com(vb,12,0)==0,"Unlock VB");
    CHECK(com(dev,118,2,9u,out)==0 && rd32(out),"Create query"); uint32_t q=rd32(out);
    bind_test_state(vs,ps,vd,t,vb); render_check(bb,q,"before loss");
    CHECK(com(dev,17,4,0u,0u,0u,0u)==0,"Present 1");
    /* Queue a query that is open at the moment the renderer is lost. */
    CHECK(com(q,6,1,2u)==0,"open query before loss");
    CHECK(com(dev,17,4,0u,0u,0u,0u)==0,"injection Present");
    /* With render-ahead the second Present can complete on the game thread
     * before the replay thread detects the failure. */
    int polls=0; uint32_t coop;
    while ((coop=com(dev,3,0))==0 && polls++<3000) usleep(1000);
    CHECK(coop==LOST,"TestCooperativeLevel reports lost (got %x)",coop);
    CHECK(com(dev,17,4,0u,0u,0u,0u)==LOST,"Present reports lost");
    if (timeout) {
        sleep(6); CHECK(com(dev,3,0)==NOTRESET,"TestCooperativeLevel reports not reset");
        CHECK(com(dev,17,4,0u,0u,0u,0u)==0,"timeout recovery Present");
    } else {
        CHECK(com(dev,16,1,pp)==0,"Reset after loss");
        CHECK(com(dev,18,4,0u,0u,0u,out)==0,"GetBackBuffer after Reset"); bb=rd32(out);
    }
    CHECK(com(dev,3,0)==0,"cooperative after recovery");
    bind_test_state(vs,ps,vd,t,vb); render_check(bb,q,"after loss");
    CHECK(com(dev,17,4,0u,0u,0u,0u)==0,"Present after recovery");
    hle_d3d9_report(); printf("test_vk_recovery (%s): %d ok, %d failed\n",timeout?"timeout":"Reset",g_ok,g_fail);
    return g_fail != 0;
}
