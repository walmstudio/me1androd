/* v0.10: GLSL -> SPIR-V for the Vulkan renderer (hle_d3d9_vk.h). A separate shared library around glslang (C API) so that
 * the runtime itself stays plain C: libme1game.so dlopens libme1spv.so when the Vulkan renderer starts.
 * me1spv_compile(stage 0 vertex / 1 fragment, GLSL source, &words, &nwords, log, logcap) -> 1 ok, 0 error (log). */
#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#define EXPORT __attribute__((visibility("default")))
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static void init(void) { glslang_initialize_process(); }
static void put(char* log, size_t cap, const char* what, const char* a, const char* b) {
    if (!log || !cap) return;
    snprintf(log, cap, "%s: %s%s%s", what, a ? a : "", b && *b ? "\n" : "", b ? b : "");
}
EXPORT int me1spv_compile(int stage, const char* src, unsigned** words, size_t* nwords, char* log, size_t logcap) {
    pthread_once(&g_once, init);
    *words = NULL; *nwords = 0; if (log && logcap) log[0] = 0;
    glslang_stage_t st = stage ? GLSLANG_STAGE_FRAGMENT : GLSLANG_STAGE_VERTEX;
    glslang_input_t in;
    memset(&in, 0, sizeof in);
    in.language = GLSLANG_SOURCE_GLSL; in.stage = st; in.client = GLSLANG_CLIENT_VULKAN; in.client_version = GLSLANG_TARGET_VULKAN_1_0;
    in.target_language = GLSLANG_TARGET_SPV; in.target_language_version = GLSLANG_TARGET_SPV_1_0;
    in.code = src; in.default_version = 310; in.default_profile = GLSLANG_ES_PROFILE;
    in.force_default_version_and_profile = 0; in.forward_compatible = 0;
    in.messages = GLSLANG_MSG_DEFAULT_BIT | GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT;
    in.resource = glslang_default_resource();
    glslang_shader_t* sh = glslang_shader_create(&in);
    if (!sh) { put(log, logcap, "glslang_shader_create failed", NULL, NULL); return 0; }
    int ok = 0;
    if (!glslang_shader_preprocess(sh, &in)) put(log, logcap, "preprocess", glslang_shader_get_info_log(sh), glslang_shader_get_info_debug_log(sh));
    else if (!glslang_shader_parse(sh, &in)) put(log, logcap, "parse", glslang_shader_get_info_log(sh), glslang_shader_get_info_debug_log(sh));
    else {
        glslang_program_t* p = glslang_program_create();
        glslang_program_add_shader(p, sh);
        if (!glslang_program_link(p, GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT))
            put(log, logcap, "link", glslang_program_get_info_log(p), glslang_program_get_info_debug_log(p));
        else {
            glslang_program_SPIRV_generate(p, st);
            size_t n = glslang_program_SPIRV_get_size(p);
            unsigned* w = n ? malloc(n * 4) : NULL;
            if (w) { glslang_program_SPIRV_get(p, w); *words = w; *nwords = n; ok = 1; }
            else put(log, logcap, "no SPIR-V generated", glslang_program_SPIRV_get_messages(p), NULL);
        }
        glslang_program_delete(p);
    }
    glslang_shader_delete(sh);
    return ok;
}
EXPORT void me1spv_free(unsigned* w) { free(w); }
