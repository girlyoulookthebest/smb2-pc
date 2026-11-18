/* OpenGL 3.3 core backend for the GX emulation core (gx_core.c, gx_gpu.h) */
#include "render.h"
#include "gx_gpu.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "perf.h"
#include "pc_settings.h"

/* OpenGL function pointer loading - use SDL's provided types from SDL_opengl_glext.h */
/* Standards-compliant cast via uintptr_t to avoid "object pointer to function pointer" warnings */
#define GL_LOAD_FUNC(func, type) do { \
    void* _ptr = SDL_GL_GetProcAddress(#func); \
    if (!_ptr) { \
        fprintf(stderr, "WARNING: Failed to load GL function: %s\n", #func); \
    } \
    func = (type)(uintptr_t)_ptr; \
} while(0)

/* For my_-prefixed pointers: ask the driver for the real GL name (#func + 3
 * skips "my_"); wglGetProcAddress returns NULL for unknown names. */
#define GL_LOAD_FUNC_MY(func, type) do { \
    void* _ptr = SDL_GL_GetProcAddress(#func + 3); \
    if (!_ptr) { \
        fprintf(stderr, "WARNING: Failed to load GL function: %s\n", #func + 3); \
    } \
    func = (type)(uintptr_t)_ptr; \
} while(0)

/* Define function pointer types for legacy functions not in SDL headers */
typedef void (APIENTRYP PFNGLDRAWARRAYSPROC) (GLenum mode, GLint first, GLsizei count);

/* Core profile functions not in legacy headers - need explicit loading */
static PFNGLGENVERTEXARRAYSPROC glGenVertexArrays = NULL;
static PFNGLBINDVERTEXARRAYPROC glBindVertexArray = NULL;
static PFNGLDELETEVERTEXARRAYSPROC glDeleteVertexArrays = NULL;

/* Shader functions - also not in legacy headers */
static PFNGLCREATESHADERPROC glCreateShader = NULL;
static PFNGLSHADERSOURCEPROC glShaderSource = NULL;
static PFNGLCOMPILESHADERPROC glCompileShader = NULL;
static PFNGLGETSHADERIVPROC glGetShaderiv = NULL;
static PFNGLGETSHADERINFOLOGPROC glGetShaderInfoLog = NULL;
static PFNGLDELETESHADERPROC glDeleteShader = NULL;
static PFNGLCREATEPROGRAMPROC glCreateProgram = NULL;
static PFNGLATTACHSHADERPROC glAttachShader = NULL;
static PFNGLLINKPROGRAMPROC glLinkProgram = NULL;
static PFNGLGETPROGRAMIVPROC glGetProgramiv = NULL;
static PFNGLGETPROGRAMINFOLOGPROC glGetProgramInfoLog = NULL;
static PFNGLUSEPROGRAMPROC glUseProgram = NULL;
static PFNGLGETUNIFORMLOCATIONPROC glGetUniformLocation = NULL;
static PFNGLUNIFORM1IPROC glUniform1i = NULL;
static PFNGLUNIFORM4FVPROC glUniform4fv = NULL;
static PFNGLUNIFORM2FPROC glUniform2f = NULL;
static PFNGLUNIFORMMATRIX4FVPROC glUniformMatrix4fv = NULL;
static PFNGLDELETEPROGRAMPROC glDeleteProgram = NULL;
static PFNGLGENERATEMIPMAPPROC glGenerateMipmap = NULL;
static PFNGLBLENDFUNCSEPARATEPROC glBlendFuncSeparate = NULL;

/* Legacy functions - need loading on Linux core profile, use different names to avoid conflicts */
static PFNGLGENBUFFERSPROC my_glGenBuffers = NULL;
static PFNGLBINDBUFFERPROC my_glBindBuffer = NULL;
static PFNGLBUFFERDATAPROC my_glBufferData = NULL;
static PFNGLBUFFERSUBDATAPROC my_glBufferSubData = NULL;
static PFNGLENABLEVERTEXATTRIBARRAYPROC my_glEnableVertexAttribArray = NULL;
static PFNGLVERTEXATTRIBPOINTERPROC my_glVertexAttribPointer = NULL;
static PFNGLDISABLEVERTEXATTRIBARRAYPROC my_glDisableVertexAttribArray = NULL;
static PFNGLDRAWARRAYSPROC my_glDrawArrays = NULL;
static PFNGLDELETEBUFFERSPROC my_glDeleteBuffers = NULL;

/* Additional functions - use my_ prefix to avoid conflict with SDL headers on Windows */
static PFNGLCOMPRESSEDTEXIMAGE2DPROC my_glCompressedTexImage2D = NULL;
static PFNGLACTIVETEXTUREPROC my_glActiveTexture = NULL;
static PFNGLBLENDEQUATIONPROC my_glBlendEquation = NULL;

static void load_gl_functions(void) {
    GL_LOAD_FUNC(glGenVertexArrays, PFNGLGENVERTEXARRAYSPROC);
    GL_LOAD_FUNC(glBindVertexArray, PFNGLBINDVERTEXARRAYPROC);
    GL_LOAD_FUNC(glDeleteVertexArrays, PFNGLDELETEVERTEXARRAYSPROC);

    GL_LOAD_FUNC_MY(my_glGenBuffers, PFNGLGENBUFFERSPROC);
    GL_LOAD_FUNC_MY(my_glBindBuffer, PFNGLBINDBUFFERPROC);
    GL_LOAD_FUNC_MY(my_glBufferData, PFNGLBUFFERDATAPROC);
    GL_LOAD_FUNC_MY(my_glBufferSubData, PFNGLBUFFERSUBDATAPROC);
    GL_LOAD_FUNC_MY(my_glEnableVertexAttribArray, PFNGLENABLEVERTEXATTRIBARRAYPROC);
    GL_LOAD_FUNC_MY(my_glVertexAttribPointer, PFNGLVERTEXATTRIBPOINTERPROC);
    GL_LOAD_FUNC_MY(my_glDisableVertexAttribArray, PFNGLDISABLEVERTEXATTRIBARRAYPROC);
    GL_LOAD_FUNC_MY(my_glDrawArrays, PFNGLDRAWARRAYSPROC);
    GL_LOAD_FUNC_MY(my_glDeleteBuffers, PFNGLDELETEBUFFERSPROC);

    GL_LOAD_FUNC(glCreateShader, PFNGLCREATESHADERPROC);
    GL_LOAD_FUNC(glShaderSource, PFNGLSHADERSOURCEPROC);
    GL_LOAD_FUNC(glCompileShader, PFNGLCOMPILESHADERPROC);
    GL_LOAD_FUNC(glGetShaderiv, PFNGLGETSHADERIVPROC);
    GL_LOAD_FUNC(glGetShaderInfoLog, PFNGLGETSHADERINFOLOGPROC);
    GL_LOAD_FUNC(glDeleteShader, PFNGLDELETESHADERPROC);
    GL_LOAD_FUNC(glCreateProgram, PFNGLCREATEPROGRAMPROC);
    GL_LOAD_FUNC(glAttachShader, PFNGLATTACHSHADERPROC);
    GL_LOAD_FUNC(glLinkProgram, PFNGLLINKPROGRAMPROC);
    GL_LOAD_FUNC(glGetProgramiv, PFNGLGETPROGRAMIVPROC);
    GL_LOAD_FUNC(glGetProgramInfoLog, PFNGLGETPROGRAMINFOLOGPROC);
    GL_LOAD_FUNC(glUseProgram, PFNGLUSEPROGRAMPROC);
    GL_LOAD_FUNC(glGetUniformLocation, PFNGLGETUNIFORMLOCATIONPROC);
    GL_LOAD_FUNC(glUniform1i, PFNGLUNIFORM1IPROC);
    GL_LOAD_FUNC(glUniform4fv, PFNGLUNIFORM4FVPROC);
    GL_LOAD_FUNC(glUniform2f, PFNGLUNIFORM2FPROC);
    GL_LOAD_FUNC(glUniformMatrix4fv, PFNGLUNIFORMMATRIX4FVPROC);
    GL_LOAD_FUNC(glDeleteProgram, PFNGLDELETEPROGRAMPROC);
    GL_LOAD_FUNC(glGenerateMipmap, PFNGLGENERATEMIPMAPPROC);
    GL_LOAD_FUNC(glBlendFuncSeparate, PFNGLBLENDFUNCSEPARATEPROC);

    GL_LOAD_FUNC_MY(my_glCompressedTexImage2D, PFNGLCOMPRESSEDTEXIMAGE2DPROC);
    GL_LOAD_FUNC_MY(my_glActiveTexture, PFNGLACTIVETEXTUREPROC);
    GL_LOAD_FUNC_MY(my_glBlendEquation, PFNGLBLENDEQUATIONPROC);

    /* Verify critical functions loaded */
    if (!my_glGenBuffers || !my_glBindBuffer || !my_glBufferData || !my_glBufferSubData ||
        !my_glEnableVertexAttribArray || !my_glVertexAttribPointer || !my_glDisableVertexAttribArray ||
        !my_glDrawArrays || !my_glDeleteBuffers) {
        fprintf(stderr, "ERROR: Critical legacy GL functions failed to load!\n");
    }
    if (!glGenVertexArrays || !glBindVertexArray || !glDeleteVertexArrays) {
        fprintf(stderr, "ERROR: Critical VAO functions failed to load!\n");
    }
    if (!my_glCompressedTexImage2D || !my_glActiveTexture || !my_glBlendEquation) {
        fprintf(stderr, "ERROR: Additional GL functions failed to load!\n");
    }
}

/* ---- client-side GL state mirror + uniform cache ----
 * With a threaded GL driver (NVIDIA's default) every glGet* / glIsEnabled
 * query drains the driver's command queue, so the per-draw path never
 * queries GL.  Only this file touches the context (the platform layer only
 * calls glReadPixels), so the mirror is exact.  Unknown pnames / caps fall
 * through to the real GL call. */
#define ST_UNKNOWN (-1)
static int s_st_cull = 0, s_st_dclamp = 0, s_st_scissor = 0; /* GL defaults */
static int s_st_blend = 0, s_st_depth = 0, s_st_stencil = 0;
static GLint s_st_vp[4] = { ST_UNKNOWN, 0, 0, 0 };
static GLint s_st_sbox[4] = { ST_UNKNOWN, 0, 0, 0 };
static GLboolean s_st_cmask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
static GLboolean s_st_dmask = GL_TRUE;
static int s_st_unit = 0;
static GLuint s_st_tex[32];

static int *st_cap(GLenum cap) {
    switch (cap) {
    case GL_CULL_FACE: return &s_st_cull;
    case GL_DEPTH_CLAMP: return &s_st_dclamp;
    case GL_SCISSOR_TEST: return &s_st_scissor;
    case GL_BLEND: return &s_st_blend;
    case GL_DEPTH_TEST: return &s_st_depth;
    case GL_STENCIL_TEST: return &s_st_stencil;
    default: return NULL;
    }
}
static void st_glEnable(GLenum cap) {
    int *m = st_cap(cap);
    if (m) { if (*m) return; *m = 1; }
    glEnable(cap);
}
static void st_glDisable(GLenum cap) {
    int *m = st_cap(cap);
    if (m) { if (!*m) return; *m = 0; }
    glDisable(cap);
}
static GLboolean st_glIsEnabled(GLenum cap) {
    int *m = st_cap(cap);
    return m ? (GLboolean)(*m != 0) : glIsEnabled(cap);
}
static void st_glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    if (s_st_vp[0] == x && s_st_vp[1] == y && s_st_vp[2] == w && s_st_vp[3] == h)
        return;
    s_st_vp[0] = x; s_st_vp[1] = y; s_st_vp[2] = w; s_st_vp[3] = h;
    glViewport(x, y, w, h);
}
static void st_glScissor(GLint x, GLint y, GLsizei w, GLsizei h) {
    if (s_st_sbox[0] == x && s_st_sbox[1] == y && s_st_sbox[2] == w && s_st_sbox[3] == h)
        return;
    s_st_sbox[0] = x; s_st_sbox[1] = y; s_st_sbox[2] = w; s_st_sbox[3] = h;
    glScissor(x, y, w, h);
}
static void st_glDepthMask(GLboolean f) {
    f = f ? GL_TRUE : GL_FALSE;
    if (f == s_st_dmask) return;
    s_st_dmask = f;
    glDepthMask(f);
}
static void st_glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
    r = r ? GL_TRUE : GL_FALSE; g = g ? GL_TRUE : GL_FALSE;
    b = b ? GL_TRUE : GL_FALSE; a = a ? GL_TRUE : GL_FALSE;
    if (s_st_cmask[0] == r && s_st_cmask[1] == g && s_st_cmask[2] == b && s_st_cmask[3] == a)
        return;
    s_st_cmask[0] = r; s_st_cmask[1] = g; s_st_cmask[2] = b; s_st_cmask[3] = a;
    glColorMask(r, g, b, a);
}
static void st_glActiveTexture(GLenum unit) {
    int u = (int)(unit - GL_TEXTURE0);
    if (u == s_st_unit) return;
    s_st_unit = u;
    my_glActiveTexture(unit);
}
static void st_glBindTexture(GLenum target, GLuint tex) {
    if (target == GL_TEXTURE_2D && s_st_unit >= 0 && s_st_unit < 32) {
        if (s_st_tex[s_st_unit] == tex) return;
        s_st_tex[s_st_unit] = tex;
    }
    glBindTexture(target, tex);
}
static void tex_size_note(GLuint id, int w, int h);
static void st_glDeleteTextures(GLsizei n, const GLuint *t) {
    GLsizei i;
    int u;
    for (i = 0; i < n; i++) tex_size_note(t[i], -1, -1);
    for (i = 0; i < n; i++)          /* deleting a bound texture unbinds it */
        for (u = 0; u < 32; u++)
            if (t[i] && s_st_tex[u] == t[i]) s_st_tex[u] = 0;
    glDeleteTextures(n, t);
}
static void st_glGetIntegerv(GLenum pname, GLint *v) {
    switch (pname) {
    case GL_VIEWPORT:
        if (s_st_vp[0] == ST_UNKNOWN) glGetIntegerv(GL_VIEWPORT, s_st_vp);
        memcpy(v, s_st_vp, sizeof(s_st_vp));
        return;
    case GL_SCISSOR_BOX:
        if (s_st_sbox[0] == ST_UNKNOWN) glGetIntegerv(GL_SCISSOR_BOX, s_st_sbox);
        memcpy(v, s_st_sbox, sizeof(s_st_sbox));
        return;
    case GL_TEXTURE_BINDING_2D:
        if (s_st_unit >= 0 && s_st_unit < 32) { *v = (GLint)s_st_tex[s_st_unit]; return; }
        break;
    default: break;
    }
    glGetIntegerv(pname, v);
}
static void st_glGetBooleanv(GLenum pname, GLboolean *v) {
    switch (pname) {
    case GL_COLOR_WRITEMASK: memcpy(v, s_st_cmask, sizeof(s_st_cmask)); return;
    case GL_DEPTH_WRITEMASK: *v = s_st_dmask; return;
    default: glGetBooleanv(pname, v); return;
    }
}
#define glEnable(c) st_glEnable(c)
#define glDisable(c) st_glDisable(c)
#define glIsEnabled(c) st_glIsEnabled(c)
#define glViewport(x, y, w, h) st_glViewport(x, y, w, h)
#define glScissor(x, y, w, h) st_glScissor(x, y, w, h)
#define glDepthMask(f) st_glDepthMask(f)
#define glColorMask(r, g, b, a) st_glColorMask(r, g, b, a)
#define my_glActiveTexture(u) st_glActiveTexture(u)
#define glBindTexture(t, x) st_glBindTexture(t, x)
#define glDeleteTextures(n, t) st_glDeleteTextures(n, t)
#define glGetIntegerv(p, v) st_glGetIntegerv(p, v)
#define glGetBooleanv(p, v) st_glGetBooleanv(p, v)

/* Uniform cache: the shader has one program, and uniform values are program
 * state, so a value equal to the last one written to a location is a no-op.
 * Most draws re-send identical matrices / TEV / fog / texgen state. */
#define UC_MAX_LOC 4096
typedef struct { unsigned n, cap; unsigned char* d; } UcEntry;
static UcEntry s_uc[UC_MAX_LOC];
static int uc_same(GLint loc, const void* p, unsigned n) {
    UcEntry* e;
    if (loc < 0) return 1;              /* GL ignores location -1 anyway */
    if (loc >= UC_MAX_LOC) return 0;
    e = &s_uc[loc];
    if (e->n == n && memcmp(e->d, p, n) == 0) return 1;
    if (e->cap < n) {
        unsigned char* nd = realloc(e->d, n);
        if (!nd) { e->n = 0; return 0; }
        e->d = nd; e->cap = n;
    }
    memcpy(e->d, p, n);
    e->n = n;
    return 0;
}
static void uc_1i(GLint loc, GLint v) {
    if (!uc_same(loc, &v, sizeof v)) glUniform1i(loc, v);
}
static void uc_2f(GLint loc, GLfloat a, GLfloat b) {
    GLfloat v[2] = { a, b };
    if (!uc_same(loc, v, sizeof v)) glUniform2f(loc, a, b);
}
static void uc_4fv(GLint loc, GLsizei n, const GLfloat* v) {
    if (!uc_same(loc, v, (unsigned)n * 4 * sizeof(GLfloat))) glUniform4fv(loc, n, v);
}
static void uc_mat4(GLint loc, GLsizei n, const GLfloat* v) {
    if (!uc_same(loc, v, (unsigned)n * 16 * sizeof(GLfloat)))
        glUniformMatrix4fv(loc, n, GL_FALSE, v);
}
typedef void (APIENTRYP UC_PFNUNIFORM4IV)(GLint, GLsizei, const GLint*);
static void uc_4iv(UC_PFNUNIFORM4IV fn, GLint loc, GLsizei n, const GLint* v) {
    if (!uc_same(loc, v, (unsigned)n * 4 * sizeof(GLint))) fn(loc, n, v);
}

static GLint s_u_texmatrix[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };

/* OpenGL backend state (the GX state itself lives in gx_core.c) */
typedef struct {
    int width;
    int height;

    /* Shaders */
    GLuint shader_program;
    GLint u_projection;
    GLint u_modelview;
    GLint u_texture[8];
    GLint u_tev_c0_loc;
    GLint u_tev_c1_loc;
    GLint u_ui_tev_loc;
    GLint u_has_tex_loc;
    GLint u_uvclamp_loc;
    GLint u_efb_scale_loc;
    /* GXFX / GXIND / texgen / fog uniforms */
    GLint u_mode, u_nrm, u_chan, u_chanfn, u_mat, u_amb;
    GLint u_lpos, u_ldir, u_lcos, u_ldist, u_lcol;
    GLint u_tev_n, u_tex_valid, u_cin, u_ain, u_cop, u_aop, u_ord, u_konst, u_reg, u_acmp;
    GLint u_ind_on, u_ind_n, u_iord, u_imtx, u_tind;
    GLint l_fog, l_fogc, l_posmask, l_srcsel, l_nrmmask, l_normmask, l_postmask, l_q1mask, l_post;
    unsigned tev_serial;   /* GxTevConsts serial last uploaded (TEV part) */
    unsigned post_serial;  /* ... (texgen post matrices) */

    /* Vertex streaming */
    GLuint vbo;
    GLuint vao;
    GLuint rect_vbo;
    int vertex_buffer_capacity;

    /* Texture bindings */
    GLuint bound_textures[8];
} GLBackend;

static GLBackend* g_backend = NULL;
static GLBackend s_gl_backend;

typedef void (APIENTRYP GXFX_PFNUNIFORM4IV)(GLint, GLsizei, const GLint*);
static GXFX_PFNUNIFORM4IV gxfx_glUniform4iv;

/* Shader source */
static const char* vertex_shader_source =
    "#version 330 core\n"
    "layout(location = 0) in vec3 a_position;\n"
    "layout(location = 1) in vec3 a_normal;\n"
    "layout(location = 2) in vec4 a_color;\n"
    "layout(location = 3) in vec2 a_texcoord0;\n"
    "layout(location = 4) in vec2 a_texcoord1;\n"
    "layout(location = 5) in vec2 a_texcoord2;\n"
    "layout(location = 6) in vec2 a_texcoord3;\n"
    "layout(location = 7) in vec4 a_uvrect;\n"
    "uniform int u_uvclamp;\n"
    "flat out vec4 v_uvrect;\n"
    "uniform mat4 u_projection;\n"
    "uniform mat4 u_modelview;\n"
    "uniform mat4 u_texmatrix[8];\n"
    "out vec4 v_color;\n"
    "out vec2 v_texcoord[8];\n"
    /* ---- begin: texgen source / fog varyings ---- */
    "uniform int u_tg_posmask;\n"   /* bit i: texgen i uses GX_TG_POS */
    "uniform int u_tg_srcsel;\n"    /* 2 bits per texgen: 0 TEX0, 1 TEX1, 2 TEX2 */
    "uniform int u_tg_nrmmask;\n"   /* bit i: texgen i uses GX_TG_NRM */
    "uniform int u_tg_normmask;\n"  /* bit i: normalize before the post matrix */
    "uniform int u_tg_postmask;\n"  /* bit i: apply the dual-tex post matrix */
    "uniform int u_tg_q1mask;\n"    /* bit i: GX_TG_MTX2x4 (q = 1) */
    "uniform mat4 u_tg_post[8];\n"  /* GX_PTTEXMTXn per texgen */
    "out vec3 v_tcq[8];\n"          /* texgen through MTX3x4: s,t,q */
    "out float v_eyez;\n"
    /* ---- end ---- */
    /* ---- GX lighting channel emulation (see GXFX section at end) ---- */
    "uniform int u_gx_mode;\n"
    "uniform mat4 u_gx_nrm;\n"
    "uniform ivec4 u_gx_chan[2];\n"     /* enable, matsrc, ambsrc, lightmask */
    "uniform ivec4 u_gx_chanfn[2];\n"   /* diffuse fn, attn fn */
    "uniform vec4 u_gx_mat;\n"
    "uniform vec4 u_gx_amb;\n"
    "uniform vec4 u_gx_lpos[8];\n"
    "uniform vec4 u_gx_ldir[8];\n"
    "uniform vec4 u_gx_lcos[8];\n"
    "uniform vec4 u_gx_ldist[8];\n"
    "uniform vec4 u_gx_lcol[8];\n"
    "vec4 gx_chan(int ch, vec3 P, vec3 N) {\n"
    "    ivec4 c = u_gx_chan[ch];\n"
    "    vec4 mat = (c.y != 0) ? a_color : u_gx_mat;\n"
    "    if (c.x == 0) return mat;\n"
    "    vec4 acc = (c.z != 0) ? a_color : u_gx_amb;\n"
    "    for (int i = 0; i < 8; i++) {\n"
    "        if ((c.w & (1 << i)) == 0) continue;\n"
    "        vec3 L = u_gx_lpos[i].xyz - P;\n"
    "        float d2 = dot(L, L);\n"
    "        float d = sqrt(d2);\n"
    "        L = L / max(d, 1e-6);\n"
    "        float nl = dot(L, N);\n"
    "        float diff = (u_gx_chanfn[ch].x == 0) ? 1.0 :\n"
    "                     (u_gx_chanfn[ch].x == 1) ? nl : max(nl, 0.0);\n"
    "        float attn = 1.0;\n"
    "        vec3 a = u_gx_lcos[i].xyz, k = u_gx_ldist[i].xyz;\n"
    "        if (u_gx_chanfn[ch].y == 1) {\n"          /* GX_AF_SPOT */
    "            float cs = max(0.0, dot(L, u_gx_ldir[i].xyz));\n"
    "            attn = max(0.0, a.x + a.y * cs + a.z * cs * cs) /\n"
    "                   max(dot(k, vec3(1.0, d, d2)), 1e-6);\n"
    "        } else if (u_gx_chanfn[ch].y == 0) {\n"   /* GX_AF_SPEC */
    "            float cs = (nl >= 0.0) ? max(0.0, dot(N, u_gx_ldir[i].xyz)) : 0.0;\n"
    "            attn = max(0.0, a.x + a.y * cs + a.z * cs * cs) /\n"
    "                   max(k.x + k.y * cs + k.z * cs * cs, 1e-6);\n"
    "        }\n"
    "        acc += diff * attn * u_gx_lcol[i];\n"
    "    }\n"
    "    return mat * clamp(acc, 0.0, 1.0);\n"
    "}\n"
    "void main() {\n"
    "    vec4 pos = u_modelview * vec4(a_position, 1.0);\n"
    "    gl_Position = u_projection * pos;\n"
    /* GX projections put clip z/w in [-1 (near), 0 (far)]; remap to GL's
     * [-1, 1] so depth 0..1 matches the EFB's 24-bit z range */
    "    gl_Position.z = 2.0 * gl_Position.z + gl_Position.w;\n"
    "    v_color = a_color;\n"
    "    if (u_gx_mode != 0) {\n"
    "        vec3 N = mat3(u_gx_nrm) * a_normal;\n"
    "        N = (dot(N, N) > 0.0) ? normalize(N) : vec3(0.0, 0.0, 1.0);\n"
    "        v_color = vec4(gx_chan(0, pos.xyz, N).rgb, gx_chan(1, pos.xyz, N).a);\n"
    "    }\n"
    "    v_eyez = -pos.z;\n"
    "    for (int i = 0; i < 8; i++) {\n"
    "        int tsel = (u_tg_srcsel >> (2 * i)) & 3;\n"
    "        vec2 tsrc = (tsel == 1) ? a_texcoord1 : ((tsel == 2) ? a_texcoord2 : a_texcoord0);\n"
    "        v_texcoord[i] = (u_texmatrix[i] * vec4(tsrc, 0.0, 1.0)).xy;\n"
    /* GX_TG_POS texgen (projected shadows).  Only the GXFX TEV
     * path reads v_tcq; a stale POS texgen must not distort the plain
     * legacy-path UV draws that follow (they get the raw UVs instead). */
    "        if (((u_tg_posmask >> i) & 1) != 0) {\n"
    "            v_tcq[i] = (u_texmatrix[i] * vec4(a_position, 1.0)).xyz;\n"
    "            if (((u_tg_q1mask >> i) & 1) != 0) v_tcq[i].z = 1.0;\n"
    "            if (u_gx_mode == 0) v_texcoord[i] = a_texcoord0;\n"
    /* GX_TG_NRM texgen (sphere/env/specular maps): the model-space normal
     * (input form ABC1) through the texgen matrix, optionally normalized,
     * then the dual-tex post matrix (GXSetTexCoordGen2).  Like POS, only
     * the GXFX TEV path reads the result. */
    "        } else if (((u_tg_nrmmask >> i) & 1) != 0) {\n"
    "            vec3 t = (u_texmatrix[i] * vec4(a_normal, 1.0)).xyz;\n"
    "            if (((u_tg_q1mask >> i) & 1) != 0) t.z = 1.0;\n"
    "            if (((u_tg_normmask >> i) & 1) != 0 && dot(t, t) > 0.0) t = normalize(t);\n"
    "            if (((u_tg_postmask >> i) & 1) != 0) t = (u_tg_post[i] * vec4(t, 1.0)).xyz;\n"
    "            v_tcq[i] = t;\n"
    "            if (u_gx_mode == 0) v_texcoord[i] = a_texcoord0;\n"
    "        } else\n"
    "            v_tcq[i] = vec3(v_texcoord[i], 1.0);\n"
    "    }\n"
    /* 2D quad UV clamp: carry the quad's UV rect into texcoord-0 space (the
     * same texmatrix as v_texcoord[0]); disabled when coord 0 isn't plain
     * TEX0 (POS texgen / other source), where the rect means nothing. */
    "    v_uvrect = vec4(-1e30, -1e30, 1e30, 1e30);\n"
    "    if (u_uvclamp != 0 && (u_tg_posmask & 1) == 0 && (u_tg_srcsel & 3) == 0) {\n"
    "        vec2 c0 = (u_texmatrix[0] * vec4(a_uvrect.xy, 0.0, 1.0)).xy;\n"
    "        vec2 c1 = (u_texmatrix[0] * vec4(a_uvrect.zy, 0.0, 1.0)).xy;\n"
    "        vec2 c2 = (u_texmatrix[0] * vec4(a_uvrect.xw, 0.0, 1.0)).xy;\n"
    "        vec2 c3 = (u_texmatrix[0] * vec4(a_uvrect.zw, 0.0, 1.0)).xy;\n"
    "        v_uvrect = vec4(min(min(c0, c1), min(c2, c3)), max(max(c0, c1), max(c2, c3)));\n"
    "    }\n"
    "}\n";

static const char* fragment_shader_source =
    "#version 330 core\n"
    "in vec4 v_color;\n"
    "in vec2 v_texcoord[8];\n"
    "uniform sampler2D u_texture[8];\n"
    "uniform vec4 u_tev_color[16];\n"
    "uniform vec4 u_tev_alpha[16];\n"
    "uniform vec4 u_tev_c0;\n"
    "uniform vec4 u_tev_c1;\n"
    "uniform int u_ui_tev;\n"
    "uniform int u_has_tex;\n"
    "out vec4 frag_color;\n"
    /* 2D quad UV clamp (see gx_flush_batch): keep texcoord 0 inside the
     * quad's UV rect inset by half an EFB (640x480) pixel's footprint, i.e.
     * within the texels the GC's outermost pixel centres sampled. */
    "flat in vec4 v_uvrect;\n"
    "uniform int u_uvclamp;\n"
    "uniform vec2 u_efb_scale;\n"   /* window pixels per EFB pixel */
    "vec2 g_uvh;\n"
    "vec2 gx_uvclamp(vec2 uv) {\n"
    "    if (u_uvclamp == 0) return uv;\n"
    "    vec2 mid = 0.5 * (v_uvrect.xy + v_uvrect.zw);\n"
    "    return clamp(uv, min(v_uvrect.xy + g_uvh, mid), max(v_uvrect.zw - g_uvh, mid));\n"
    "}\n"
    /* ---- GX TEV emulation (see GXFX section at end) ---- */
    "uniform int u_gx_mode;\n"
    "uniform int u_gx_tev_n;\n"
    "uniform int u_gx_tex_valid;\n"
    "uniform ivec4 u_gx_cin[8];\n"
    "uniform ivec4 u_gx_ain[8];\n"
    "uniform ivec4 u_gx_cop[8];\n"      /* op, bias, scale, clamp|outreg<<1 */
    "uniform ivec4 u_gx_aop[8];\n"
    "uniform ivec4 u_gx_ord[8];\n"      /* texcoord, texmap(-1 none), raster on */
    "uniform vec4 u_gx_konst[8];\n"
    "uniform vec4 u_gx_reg[4];\n"
    "uniform ivec4 u_gx_acmp;\n"        /* comp0, comp1, op, ref0|ref1<<8 */
    /* ---- GX indirect texturing (see GXIND section at end) ---- */
    "uniform int u_gx_ind_on;\n"       /* bit s: TEV stage s has an indirect setup */
    "uniform int u_gx_ind_n;\n"        /* GXSetNumIndStages */
    "uniform ivec4 u_gx_iord[4];\n"    /* ind stage: texcoord, texmap, scaleS, scaleT */
    "uniform vec4 u_gx_imtx[6];\n"     /* ITM_0..2: row0 (m00,m01,m02,scaleExp), row1 */
    "uniform ivec4 u_gx_tind[8];\n"    /* bt|fmt<<2|bias<<4|bumpa<<7, mtx, wrapS|wrapT<<3, addprev */
    /* ---- begin: perspective texgen + GX fog ---- */
    "in vec3 v_tcq[8];\n"
    "in float v_eyez;\n"
    "uniform vec4 u_fog;\n"          /* type, start, end, 0 */
    "uniform vec4 u_fog_color;\n"
    "vec2 gx_tcq(vec3 q) { return q.xy / ((abs(q.z) > 1e-6) ? q.z : 1e-6); }\n"
    "vec2 gx_tc(int c) {\n"
    "    if (c == 0) return gx_tcq(v_tcq[0]); if (c == 1) return gx_tcq(v_tcq[1]);\n"
    "    if (c == 2) return gx_tcq(v_tcq[2]); if (c == 3) return gx_tcq(v_tcq[3]);\n"
    "    if (c == 4) return gx_tcq(v_tcq[4]); if (c == 5) return gx_tcq(v_tcq[5]);\n"
    "    if (c == 6) return gx_tcq(v_tcq[6]); return gx_tcq(v_tcq[7]);\n"
    "}\n"
    "vec4 gx_fog(vec4 c) {\n"
    "    if (u_fog.x < 0.5) return c;\n"
    "    int ft = int(u_fog.x + 0.5) & 7;\n"
    "    float t = clamp((v_eyez - u_fog.y) / max(u_fog.z - u_fog.y, 1e-4), 0.0, 1.0);\n"
    "    float f = t;\n"
    "    if (ft == 4) f = 1.0 - exp2(-8.0 * t);\n"
    "    else if (ft == 5) f = 1.0 - exp2(-8.0 * t * t);\n"
    "    else if (ft == 6) f = exp2(-8.0 * (1.0 - t));\n"
    "    else if (ft == 7) f = exp2(-8.0 * (1.0 - t) * (1.0 - t));\n"
    "    c.rgb = mix(c.rgb, u_fog_color.rgb, clamp(f, 0.0, 1.0));\n"
    "    return c;\n"
    "}\n"
    /* ---- end ---- */
    "vec4 gx_tex(int m, vec2 uv) {\n"
    "    if (m < 0 || m > 7 || (u_gx_tex_valid & (1 << m)) == 0) return vec4(1.0);\n"
    "    if (m == 0) return texture(u_texture[0], uv); if (m == 1) return texture(u_texture[1], uv);\n"
    "    if (m == 2) return texture(u_texture[2], uv); if (m == 3) return texture(u_texture[3], uv);\n"
    "    if (m == 4) return texture(u_texture[4], uv); if (m == 5) return texture(u_texture[5], uv);\n"
    "    if (m == 6) return texture(u_texture[6], uv); return texture(u_texture[7], uv);\n"
    "}\n"
    "vec2 gx_tsize(int m) {\n"
    "    if (m == 0) return vec2(textureSize(u_texture[0], 0)); if (m == 1) return vec2(textureSize(u_texture[1], 0));\n"
    "    if (m == 2) return vec2(textureSize(u_texture[2], 0)); if (m == 3) return vec2(textureSize(u_texture[3], 0));\n"
    "    if (m == 4) return vec2(textureSize(u_texture[4], 0)); if (m == 5) return vec2(textureSize(u_texture[5], 0));\n"
    "    if (m == 6) return vec2(textureSize(u_texture[6], 0)); return vec2(textureSize(u_texture[7], 0));\n"
    "}\n"
    /* GX indirect stage (Dolphin TevShaderGen semantics).  Coordinates are
     * in texels of the stage's own texture; acc is the running tevcoord
     * that GX_TRUE add-prev stages accumulate into. */
    "float gx_iwrap(float c, int w) {\n"
    "    if (w == 0) return c; if (w >= 6) return 0.0;\n"
    "    float t = float(512 >> w); return c - t * floor(c / t);\n"
    "}\n"
    "vec2 gx_ind_uv(int s, int m, vec2 uv, inout vec2 acc) {\n"
    "    vec2 sz = max(gx_tsize(m), vec2(1.0));\n"
    "    vec2 fc = uv * sz;\n"
    "    if (((u_gx_ind_on >> s) & 1) == 0) { acc = fc; return uv; }\n"
    "    ivec4 ti = u_gx_tind[s];\n"
    "    int bt = ti.x & 3, fmt = (ti.x >> 2) & 3, bias = (ti.x >> 4) & 7;\n"
    "    vec3 ind = vec3(0.0);\n"
    "    if (bt < u_gx_ind_n) {\n"
    "        ivec4 io = u_gx_iord[bt];\n"
    "        vec2 ic = gx_tc(io.x) / vec2(float(1 << io.z), float(1 << io.w));\n"
    "        ivec3 ii = ivec3(floor(gx_tex(io.y, ic).abg * 255.0 + 0.5));\n"
    "        int mask = (fmt == 0) ? 255 : (fmt == 1) ? 31 : (fmt == 2) ? 15 : 7;\n"
    "        int badd = (fmt == 0) ? -128 : 1;\n"
    "        ii &= ivec3(mask);\n"
    "        if ((bias & 1) != 0) ii.x += badd;\n"
    "        if ((bias & 2) != 0) ii.y += badd;\n"
    "        if ((bias & 4) != 0) ii.z += badd;\n"
    "        ind = vec3(ii);\n"
    "    }\n"
    "    vec2 tr = vec2(0.0);\n"
    "    int mid = ti.y;\n"
    "    if (mid >= 1 && mid <= 3) {\n"
    "        vec4 r0 = u_gx_imtx[(mid - 1) * 2], r1 = u_gx_imtx[(mid - 1) * 2 + 1];\n"
    "        tr = vec2(dot(r0.xyz, ind), dot(r1.xyz, ind)) * exp2(r0.w);\n"
    "    } else if ((mid >= 5 && mid <= 7) || (mid >= 9 && mid <= 11)) {\n"
    "        float e = u_gx_imtx[((mid & 3) - 1) * 2].w;\n"
    "        tr = fc * ((mid < 8) ? ind.x : ind.y) / 256.0 * exp2(e);\n"
    "    }\n"
    "    vec2 wc = vec2(gx_iwrap(fc.x, ti.z & 7), gx_iwrap(fc.y, (ti.z >> 3) & 7));\n"
    "    acc = (ti.w != 0) ? acc + wc + tr : wc + tr;\n"
    "    return acc / sz;\n"
    "}\n"
    "vec3 gx_cin(int i, vec4 r0, vec4 r1, vec4 r2, vec4 r3, vec4 t, vec4 ras, vec4 k) {\n"
    "    if (i == 0) return r0.rgb; if (i == 1) return vec3(r0.a);\n"
    "    if (i == 2) return r1.rgb; if (i == 3) return vec3(r1.a);\n"
    "    if (i == 4) return r2.rgb; if (i == 5) return vec3(r2.a);\n"
    "    if (i == 6) return r3.rgb; if (i == 7) return vec3(r3.a);\n"
    "    if (i == 8) return t.rgb; if (i == 9) return vec3(t.a);\n"
    "    if (i == 10) return ras.rgb; if (i == 11) return vec3(ras.a);\n"
    "    if (i == 12) return vec3(1.0); if (i == 13) return vec3(0.5);\n"
    "    if (i == 14) return k.rgb; return vec3(0.0);\n"
    "}\n"
    "float gx_ain(int i, vec4 r0, vec4 r1, vec4 r2, vec4 r3, vec4 t, vec4 ras, vec4 k) {\n"
    "    if (i == 0) return r0.a; if (i == 1) return r1.a; if (i == 2) return r2.a;\n"
    "    if (i == 3) return r3.a; if (i == 4) return t.a; if (i == 5) return ras.a;\n"
    "    if (i == 6) return k.a; return 0.0;\n"
    "}\n"
    "float gx_bias(int b) { return b == 1 ? 0.5 : (b == 2 ? -0.5 : 0.0); }\n"
    "float gx_scale(int s) { return s == 1 ? 2.0 : (s == 2 ? 4.0 : (s == 3 ? 0.5 : 1.0)); }\n"
    "bool gx_acmp(int f, float a, float ref) {\n"
    "    if (f == 0) return false; if (f == 1) return a < ref; if (f == 2) return a == ref;\n"
    "    if (f == 3) return a <= ref; if (f == 4) return a > ref; if (f == 5) return a != ref;\n"
    "    if (f == 6) return a >= ref; return true;\n"
    "}\n"
    "bool gx_alpha_pass(float a8) {\n"
    "    bool p0 = gx_acmp(u_gx_acmp.x, a8, float(u_gx_acmp.w & 255));\n"
    "    bool p1 = gx_acmp(u_gx_acmp.y, a8, float((u_gx_acmp.w >> 8) & 255));\n"
    "    return (u_gx_acmp.z == 0) ? (p0 && p1) : (u_gx_acmp.z == 1) ? (p0 || p1) :\n"
    "           (u_gx_acmp.z == 2) ? (p0 != p1) : (p0 == p1);\n"
    "}\n"
    "vec4 gx_swap(vec4 v, int sw) {\n"
    "    return vec4(v[sw & 3], v[(sw >> 2) & 3], v[(sw >> 4) & 3], v[(sw >> 6) & 3]);\n"
    "}\n"
    "vec4 gx_tev() {\n"
    "    vec4 r0 = u_gx_reg[0], r1 = u_gx_reg[1], r2 = u_gx_reg[2], r3 = u_gx_reg[3];\n"
    "    vec4 last = v_color;\n"
    "    vec2 iacc = vec2(0.0);\n"
    "    for (int s = 0; s < u_gx_tev_n; s++) {\n"
    "        ivec4 o = u_gx_ord[s];\n"
    "        vec2 uv = gx_tc(o.x);\n"
    "        if (o.x == 0 && ((u_gx_ind_on >> s) & 1) == 0) uv = gx_uvclamp(uv);\n"
    "        if (u_gx_ind_on != 0 && o.y >= 0) uv = gx_ind_uv(s, o.y, uv, iacc);\n"
    "        vec4 t = (o.y >= 0) ? gx_tex(o.y, uv) : vec4(1.0);\n"
    "        vec4 ras = (o.z != 0) ? v_color : vec4(0.0);\n"
    "        if (o.w != 0) {  /* TEV swap tables (gx_core gxfx_swap_bits) */\n"
    "            t = gx_swap(t, (o.w & 255) ^ 228);\n"
    "            ras = gx_swap(ras, ((o.w >> 8) & 255) ^ 228);\n"
    "        }\n"
    "        vec4 k = u_gx_konst[s];\n"
    "        ivec4 ci = u_gx_cin[s], ai = u_gx_ain[s], co = u_gx_cop[s], ao = u_gx_aop[s];\n"
    "        vec3 ca = gx_cin(ci.x, r0, r1, r2, r3, t, ras, k);\n"
    "        vec3 cb = gx_cin(ci.y, r0, r1, r2, r3, t, ras, k);\n"
    "        vec3 cc = gx_cin(ci.z, r0, r1, r2, r3, t, ras, k);\n"
    "        vec3 cd = gx_cin(ci.w, r0, r1, r2, r3, t, ras, k);\n"
    "        float aa = gx_ain(ai.x, r0, r1, r2, r3, t, ras, k);\n"
    "        float ab = gx_ain(ai.y, r0, r1, r2, r3, t, ras, k);\n"
    "        float ac = gx_ain(ai.z, r0, r1, r2, r3, t, ras, k);\n"
    "        float ad = gx_ain(ai.w, r0, r1, r2, r3, t, ras, k);\n"
    "        vec3 cm = mix(ca, cb, cc);\n"
    "        float am = mix(aa, ab, ac);\n"
    "        vec3 cr = (cd + ((co.x == 1) ? -cm : cm) + gx_bias(co.y)) * gx_scale(co.z);\n"
    "        float ar = (ad + ((ao.x == 1) ? -am : am) + gx_bias(ao.y)) * gx_scale(ao.z);\n"
    "        cr = ((co.w & 1) != 0) ? clamp(cr, 0.0, 1.0) : clamp(cr, -4.0, 4.0);\n"
    "        ar = ((ao.w & 1) != 0) ? clamp(ar, 0.0, 1.0) : clamp(ar, -4.0, 4.0);\n"
    "        int cdst = co.w >> 1, adst = ao.w >> 1;\n"
    "        if (cdst == 0) r0.rgb = cr; else if (cdst == 1) r1.rgb = cr;\n"
    "        else if (cdst == 2) r2.rgb = cr; else r3.rgb = cr;\n"
    "        if (adst == 0) r0.a = ar; else if (adst == 1) r1.a = ar;\n"
    "        else if (adst == 2) r2.a = ar; else r3.a = ar;\n"
    "        last = vec4(cr, ar);\n"
    "    }\n"
    "    last = clamp(last, 0.0, 1.0);\n"
    "    {\n"
    "        float a8 = floor(last.a * 255.0 + 0.5);\n"
    "        if (!gx_alpha_pass(a8)) discard;\n"
    "    }\n"
    "    return last;\n"
    "}\n"
    "void main() {\n"
    "    g_uvh = vec2(0.0);\n"
    "    if (u_uvclamp != 0) {\n"
    "        vec2 dx = dFdx(v_texcoord[0]), dy = dFdy(v_texcoord[0]);\n"
    "        g_uvh = 0.5 * (abs(dx) * u_efb_scale.x + abs(dy) * u_efb_scale.y);\n"
    "    }\n"
    "    frag_color = v_color;\n"
    "    if (u_gx_mode != 0 && u_ui_tev == 0) { frag_color = gx_fog(gx_tev()); return; }\n" /* fog */
    "    if (u_has_tex != 0) {\n"
    "        vec4 tex = texture(u_texture[0], gx_uvclamp(v_texcoord[0]));\n"
    "        if (u_ui_tev != 0) {\n"
    "            frag_color = clamp(tex * u_tev_c0 + u_tev_c1, 0.0, 1.0);\n"
    /* GX alpha compare runs on the TEV output for every fragment; the 2D
     * sprite/text path relies on it (GREATER 0) so the transparent part of
     * a glyph/sprite quad neither shows nor writes depth (overlapping
     * proportional glyphs would clip each other). */
    "            if (!gx_alpha_pass(floor(frag_color.a * 255.0 + 0.5))) discard;\n"
    "        } else\n"
    "            frag_color *= tex;\n"
    "    }\n"
    "    frag_color = gx_fog(frag_color);\n"
    "}\n";

static GLuint compile_shader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(shader, 512, NULL, infoLog);
        fprintf(stderr, "Shader compilation failed: %s\n", infoLog);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}
/* Streaming vertex upload.  Rewriting a buffer range a previous draw still
 * reads makes the driver serialize the GPU, so batches go to successive
 * ranges of a ring and are drawn with first/basevertex = their ring offset.
 *
 * Fast path (GL_ARB_buffer_storage): the ring is persistently mapped and
 * coherent, split into 4 quarters with a fence each so the CPU never
 * overwrites vertices the GPU has not consumed yet.  Fallback: the same ring
 * through glBufferSubData, orphaning both buffers on wrap. */
typedef void (APIENTRYP RING_PFNDRAWELEMENTSBASEVERTEX)(GLenum, GLsizei, GLenum,
                                                        const void*, GLint);
typedef void (APIENTRYP RING_PFNBUFFERSTORAGE)(GLenum, GLsizeiptr, const void*, GLbitfield);
typedef void* (APIENTRYP RING_PFNMAPBUFFERRANGE)(GLenum, GLintptr, GLsizeiptr, GLbitfield);
typedef GLsync (APIENTRYP RING_PFNFENCESYNC)(GLenum, GLbitfield);
typedef GLenum (APIENTRYP RING_PFNCLIENTWAITSYNC)(GLsync, GLbitfield, GLuint64);
typedef void (APIENTRYP RING_PFNDELETESYNC)(GLsync);
#ifndef GL_MAP_PERSISTENT_BIT
#define GL_MAP_PERSISTENT_BIT 0x0040
#endif
#ifndef GL_MAP_COHERENT_BIT
#define GL_MAP_COHERENT_BIT 0x0080
#endif
static RING_PFNDRAWELEMENTSBASEVERTEX s_glDrawElementsBaseVertex;
static RING_PFNFENCESYNC s_glFenceSync;
static RING_PFNCLIENTWAITSYNC s_glClientWaitSync;
static RING_PFNDELETESYNC s_glDeleteSync;
#define RING_PERSIST_VERTS (4 * 65536)  /* 16 MB of vertices + 4 MB of rects */
#define RING_QUARTERS 4
static int s_ring_init, s_ring_pos, s_ring_cap, s_ring_q;
static float* s_ring_vmap;   /* persistent mapping of gl->vbo (16 floats/vert) */
static float* s_ring_rmap;   /* persistent mapping of gl->rect_vbo (4 floats/vert) */
static GLsync s_ring_fence[RING_QUARTERS];

static void ring_vao_attribs(GLBackend* gl) {
    static const struct { int n, off; } a[7] = {
        {3, 0}, {3, 3}, {4, 6}, {2, 10}, {2, 12}, {2, 14}, {2, 16}
    };
    int i;
    my_glBindBuffer(GL_ARRAY_BUFFER, gl->vbo);
    for (i = 0; i < 7; i++)
        my_glVertexAttribPointer(i, a[i].n, GL_FLOAT, GL_FALSE, 16 * sizeof(float),
                                 (void*)(uintptr_t)(a[i].off * sizeof(float)));
    my_glBindBuffer(GL_ARRAY_BUFFER, gl->rect_vbo);
    my_glVertexAttribPointer(7, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
}

/* called with gl->vao bound */
static void ring_setup(GLBackend* gl) {
    RING_PFNBUFFERSTORAGE bs;
    RING_PFNMAPBUFFERRANGE map;
    GLuint vbo = 0, rvbo = 0;
    GLbitfield fl = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    s_ring_init = 1;
    s_ring_cap = gl->vertex_buffer_capacity / 16;
    s_glDrawElementsBaseVertex = (RING_PFNDRAWELEMENTSBASEVERTEX)(uintptr_t)
        SDL_GL_GetProcAddress("glDrawElementsBaseVertex");
    if (!s_glDrawElementsBaseVertex) return;
    if (getenv("SMB2_NO_PERSISTENT_VBO") || !gl->rect_vbo ||
        !SDL_GL_ExtensionSupported("GL_ARB_buffer_storage"))
        return;
    bs = (RING_PFNBUFFERSTORAGE)(uintptr_t)SDL_GL_GetProcAddress("glBufferStorage");
    map = (RING_PFNMAPBUFFERRANGE)(uintptr_t)SDL_GL_GetProcAddress("glMapBufferRange");
    s_glFenceSync = (RING_PFNFENCESYNC)(uintptr_t)SDL_GL_GetProcAddress("glFenceSync");
    s_glClientWaitSync = (RING_PFNCLIENTWAITSYNC)(uintptr_t)SDL_GL_GetProcAddress("glClientWaitSync");
    s_glDeleteSync = (RING_PFNDELETESYNC)(uintptr_t)SDL_GL_GetProcAddress("glDeleteSync");
    if (!bs || !map || !s_glFenceSync || !s_glClientWaitSync || !s_glDeleteSync) return;
    my_glGenBuffers(1, &vbo);
    my_glGenBuffers(1, &rvbo);
    my_glBindBuffer(GL_ARRAY_BUFFER, vbo);
    bs(GL_ARRAY_BUFFER, (GLsizeiptr)RING_PERSIST_VERTS * 16 * sizeof(float), NULL, fl);
    s_ring_vmap = (float*)map(GL_ARRAY_BUFFER, 0,
                              (GLsizeiptr)RING_PERSIST_VERTS * 16 * sizeof(float), fl);
    my_glBindBuffer(GL_ARRAY_BUFFER, rvbo);
    bs(GL_ARRAY_BUFFER, (GLsizeiptr)RING_PERSIST_VERTS * 4 * sizeof(float), NULL, fl);
    s_ring_rmap = (float*)map(GL_ARRAY_BUFFER, 0,
                              (GLsizeiptr)RING_PERSIST_VERTS * 4 * sizeof(float), fl);
    if (!s_ring_vmap || !s_ring_rmap) {
        s_ring_vmap = s_ring_rmap = NULL;
        my_glDeleteBuffers(1, &vbo);
        my_glDeleteBuffers(1, &rvbo);
        my_glBindBuffer(GL_ARRAY_BUFFER, gl->vbo);
        fprintf(stderr, "[gl] persistent vertex ring unavailable, using glBufferSubData\n");
        return;
    }
    my_glDeleteBuffers(1, &gl->vbo);
    my_glDeleteBuffers(1, &gl->rect_vbo);
    gl->vbo = vbo;
    gl->rect_vbo = rvbo;
    s_ring_cap = RING_PERSIST_VERTS;
    ring_vao_attribs(gl);
    my_glBindBuffer(GL_ARRAY_BUFFER, gl->vbo);
    fprintf(stderr, "[gl] persistent vertex ring: %d verts\n", s_ring_cap);
}

/* Reserve n consecutive ring vertices (n <= MAX_BATCH_VERTS); gl->vbo bound.
 * Returns the first vertex index. */
static int ring_alloc(GLBackend* gl, int n) {
    int base;
    if (!s_ring_init) ring_setup(gl);
    if (!s_glDrawElementsBaseVertex) return 0; /* no ring: always offset 0 */
    if (s_ring_vmap) {
        int qsize = s_ring_cap / RING_QUARTERS, q;
        if (s_ring_pos % qsize + n > qsize)        /* keep a batch in one quarter */
            s_ring_pos = (s_ring_pos / qsize + 1) * qsize;
        if (s_ring_pos >= s_ring_cap) s_ring_pos = 0;
        q = s_ring_pos / qsize;
        if (q != s_ring_q) {
            /* leaving quarter s_ring_q: fence the draws that read it; entering
             * q: wait until the GPU is done with its previous lap */
            if (s_ring_fence[s_ring_q]) s_glDeleteSync(s_ring_fence[s_ring_q]);
            s_ring_fence[s_ring_q] = s_glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            if (s_ring_fence[q]) {
                while (s_glClientWaitSync(s_ring_fence[q], GL_SYNC_FLUSH_COMMANDS_BIT,
                                          1000000000ull) == GL_TIMEOUT_EXPIRED) {}
                s_glDeleteSync(s_ring_fence[q]);
                s_ring_fence[q] = 0;
            }
            s_ring_q = q;
        }
    } else if (s_ring_pos + n > s_ring_cap) {
        my_glBufferData(GL_ARRAY_BUFFER, (size_t)gl->vertex_buffer_capacity * sizeof(float),
                        NULL, GL_DYNAMIC_DRAW);
        if (gl->rect_vbo) {
            my_glBindBuffer(GL_ARRAY_BUFFER, gl->rect_vbo);
            my_glBufferData(GL_ARRAY_BUFFER, (size_t)65536 * 4 * sizeof(float), NULL,
                            GL_DYNAMIC_DRAW);
            my_glBindBuffer(GL_ARRAY_BUFFER, gl->vbo);
        }
        s_ring_pos = 0;
    }
    base = s_ring_pos;
    s_ring_pos += n;
    return base;
}

/* write n vertices (16 floats each) at ring index base; gl->vbo bound */
static void ring_write(int base, const float* v, int n) {
    if (s_ring_vmap)
        memcpy(s_ring_vmap + (size_t)base * 16, v, (size_t)n * 16 * sizeof(float));
    else
        my_glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)base * 16 * sizeof(float),
                           (GLsizeiptr)n * 16 * sizeof(float), v);
}

/* write n quad UV rects (4 floats each) at ring index base; gl->vbo bound */
static void ring_write_rect(GLBackend* gl, int base, const float* r, int n) {
    if (s_ring_rmap) {
        memcpy(s_ring_rmap + (size_t)base * 4, r, (size_t)n * 4 * sizeof(float));
        return;
    }
    my_glBindBuffer(GL_ARRAY_BUFFER, gl->rect_vbo);
    my_glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)base * 4 * sizeof(float),
                       (GLsizeiptr)n * 4 * sizeof(float), r);
    my_glBindBuffer(GL_ARRAY_BUFFER, gl->vbo);
}

static bool init_shaders(GLBackend* backend) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vertex_shader_source);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fragment_shader_source);
    GLuint prog;
    if (!vs || !fs) return false;

    backend->shader_program = glCreateProgram();
    glAttachShader(backend->shader_program, vs);
    glAttachShader(backend->shader_program, fs);
    glLinkProgram(backend->shader_program);

    GLint success;
    glGetProgramiv(backend->shader_program, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(backend->shader_program, 512, NULL, infoLog);
        fprintf(stderr, "Shader program linking failed: %s\n", infoLog);
        return false;
    }

    glDeleteShader(vs);
    glDeleteShader(fs);

    /* Get uniform locations */
    prog = backend->shader_program;
    glUseProgram(prog);
    backend->u_projection = glGetUniformLocation(prog, "u_projection");
    backend->u_modelview = glGetUniformLocation(prog, "u_modelview");
    for (int i = 0; i < 8; i++) {
        char name[32];
        snprintf(name, sizeof(name), "u_texture[%d]", i);
        backend->u_texture[i] = glGetUniformLocation(prog, name);
        glUniform1i(backend->u_texture[i], i);
    }
    backend->u_tev_c0_loc = glGetUniformLocation(prog, "u_tev_c0");
    backend->u_tev_c1_loc = glGetUniformLocation(prog, "u_tev_c1");
    backend->u_ui_tev_loc = glGetUniformLocation(prog, "u_ui_tev");
    backend->u_has_tex_loc = glGetUniformLocation(prog, "u_has_tex");
    backend->u_uvclamp_loc = glGetUniformLocation(prog, "u_uvclamp");
    backend->u_efb_scale_loc = glGetUniformLocation(prog, "u_efb_scale");
    for (int i = 0; i < 8; i++) {
        char name[24];
        snprintf(name, sizeof(name), "u_texmatrix[%d]", i);
        s_u_texmatrix[i] = glGetUniformLocation(prog, name);
    }
    gxfx_glUniform4iv = (GXFX_PFNUNIFORM4IV)(uintptr_t)SDL_GL_GetProcAddress("glUniform4iv");
    backend->u_mode = glGetUniformLocation(prog, "u_gx_mode");
    backend->u_nrm = glGetUniformLocation(prog, "u_gx_nrm");
    backend->u_chan = glGetUniformLocation(prog, "u_gx_chan");
    backend->u_chanfn = glGetUniformLocation(prog, "u_gx_chanfn");
    backend->u_mat = glGetUniformLocation(prog, "u_gx_mat");
    backend->u_amb = glGetUniformLocation(prog, "u_gx_amb");
    backend->u_lpos = glGetUniformLocation(prog, "u_gx_lpos");
    backend->u_ldir = glGetUniformLocation(prog, "u_gx_ldir");
    backend->u_lcos = glGetUniformLocation(prog, "u_gx_lcos");
    backend->u_ldist = glGetUniformLocation(prog, "u_gx_ldist");
    backend->u_lcol = glGetUniformLocation(prog, "u_gx_lcol");
    backend->u_tev_n = glGetUniformLocation(prog, "u_gx_tev_n");
    backend->u_tex_valid = glGetUniformLocation(prog, "u_gx_tex_valid");
    backend->u_cin = glGetUniformLocation(prog, "u_gx_cin");
    backend->u_ain = glGetUniformLocation(prog, "u_gx_ain");
    backend->u_cop = glGetUniformLocation(prog, "u_gx_cop");
    backend->u_aop = glGetUniformLocation(prog, "u_gx_aop");
    backend->u_ord = glGetUniformLocation(prog, "u_gx_ord");
    backend->u_konst = glGetUniformLocation(prog, "u_gx_konst");
    backend->u_reg = glGetUniformLocation(prog, "u_gx_reg");
    backend->u_acmp = glGetUniformLocation(prog, "u_gx_acmp");
    backend->u_ind_on = glGetUniformLocation(prog, "u_gx_ind_on");
    backend->u_ind_n = glGetUniformLocation(prog, "u_gx_ind_n");
    backend->u_iord = glGetUniformLocation(prog, "u_gx_iord");
    backend->u_imtx = glGetUniformLocation(prog, "u_gx_imtx");
    backend->u_tind = glGetUniformLocation(prog, "u_gx_tind");
    backend->l_fog = glGetUniformLocation(prog, "u_fog");
    backend->l_fogc = glGetUniformLocation(prog, "u_fog_color");
    backend->l_posmask = glGetUniformLocation(prog, "u_tg_posmask");
    backend->l_srcsel = glGetUniformLocation(prog, "u_tg_srcsel");
    backend->l_nrmmask = glGetUniformLocation(prog, "u_tg_nrmmask");
    backend->l_normmask = glGetUniformLocation(prog, "u_tg_normmask");
    backend->l_postmask = glGetUniformLocation(prog, "u_tg_postmask");
    backend->l_q1mask = glGetUniformLocation(prog, "u_tg_q1mask");
    backend->l_post = glGetUniformLocation(prog, "u_tg_post");
    {   /* the initial GX state's UI TEV colours (core: C0 = 1, C1 = 0) */
        static const float one[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        uc_4fv(backend->u_tev_c0_loc, 1, one);
    }
    return true;
}

static bool glb_init(int width, int height, bool vsync, int msaa) {
    printf("DEBUG: gl_init started\n");
    fflush(stdout);

    /* Load OpenGL function pointers */
    load_gl_functions();

    GLBackend* gl = &s_gl_backend;
    memset(gl, 0, sizeof *gl);

    gl->width = width;
    gl->height = height;
    (void)vsync; (void)msaa;   /* chosen with the GL context (platform layer) */

    if (!init_shaders(gl)) {
        return false;
    }

    /* Create VAO and VBO for immediate mode */
    glGenVertexArrays(1, &gl->vao);
    my_glGenBuffers(1, &gl->vbo);
    glBindVertexArray(gl->vao);
    my_glBindBuffer(GL_ARRAY_BUFFER, gl->vbo);

    gl->vertex_buffer_capacity = 65536 * 16; /* 16 floats per vertex */
    my_glBufferData(GL_ARRAY_BUFFER, gl->vertex_buffer_capacity * sizeof(float), NULL, GL_DYNAMIC_DRAW);

    /* Position */
    my_glEnableVertexAttribArray(0);
    my_glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 16 * sizeof(float), (void*)0);
    /* Normal */
    my_glEnableVertexAttribArray(1);
    my_glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 16 * sizeof(float), (void*)(3 * sizeof(float)));
    /* Color */
    my_glEnableVertexAttribArray(2);
    my_glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 16 * sizeof(float), (void*)(6 * sizeof(float)));
    /* TexCoord0 */
    my_glEnableVertexAttribArray(3);
    my_glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, 16 * sizeof(float), (void*)(10 * sizeof(float)));
    /* TexCoord1 */
    my_glEnableVertexAttribArray(4);
    my_glVertexAttribPointer(4, 2, GL_FLOAT, GL_FALSE, 16 * sizeof(float), (void*)(12 * sizeof(float)));
    /* TexCoord2 */
    my_glEnableVertexAttribArray(5);
    my_glVertexAttribPointer(5, 2, GL_FLOAT, GL_FALSE, 16 * sizeof(float), (void*)(14 * sizeof(float)));
    /* TexCoord3 */
    my_glEnableVertexAttribArray(6);
    my_glVertexAttribPointer(6, 2, GL_FLOAT, GL_FALSE, 16 * sizeof(float), (void*)(16 * sizeof(float)));
    /* Quad UV rect (2D quad UV clamp), its own tightly packed buffer */
    my_glGenBuffers(1, &gl->rect_vbo);
    my_glBindBuffer(GL_ARRAY_BUFFER, gl->rect_vbo);
    my_glBufferData(GL_ARRAY_BUFFER, (size_t)65536 * 4 * sizeof(float), NULL, GL_DYNAMIC_DRAW);
    my_glEnableVertexAttribArray(7);
    my_glVertexAttribPointer(7, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);

    glBindVertexArray(0);
    my_glBindBuffer(GL_ARRAY_BUFFER, 0);

    g_backend = gl;

    /* OpenGL global state */
    glEnable(GL_DEPTH_TEST);
#ifndef GL_DEPTH_CLAMP
#define GL_DEPTH_CLAMP 0x864F
#endif
    /* GC rasterizes near-plane-hugging geometry (the game draws
     * full-screen quads at ~0.99*near); GL would clip them away */
    glEnable(GL_DEPTH_CLAMP);
    glEnable(GL_CULL_FACE);
    /* GX front faces are clockwise in GL's y-up NDC: SMB1/SMB2 avdisp cull
     * API GX_CULL_FRONT on display list 0, whose geometry is CCW in NDC,
     * and NL models (nl2ngc) cull GX_CULL_BACK on CW geometry. */
    glFrontFace(GL_CW);
    glCullFace(GL_BACK);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    printf("OpenGL render backend initialized: %dx%d\n", width, height);
    printf("GL Vendor: %s\n", glGetString(GL_VENDOR));
    printf("GL Renderer: %s\n", glGetString(GL_RENDERER));
    printf("GL Version: %s\n", glGetString(GL_VERSION));
    printf("GLSL Version: %s\n", glGetString(GL_SHADING_LANGUAGE_VERSION));

    return true;
}

static void glb_shutdown(void) {
    GLBackend* gl = g_backend;
    if (!gl) return;
    if (gl->vbo) my_glDeleteBuffers(1, &gl->vbo);
    if (gl->rect_vbo) my_glDeleteBuffers(1, &gl->rect_vbo);
    if (gl->vao) glDeleteVertexArrays(1, &gl->vao);
    if (gl->shader_program) glDeleteProgram(gl->shader_program);
    g_backend = NULL;
}

static void glb_begin_frame(void) {
    if (g_backend) glUseProgram(g_backend->shader_program);
}

static void glb_resize(int width, int height) {
    if (!g_backend) return;
    g_backend->width = width;
    g_backend->height = height;
}

static void glb_present(void) {
    SDL_Window* w = SDL_GL_GetCurrentWindow();
    if (w) SDL_GL_SwapWindow(w);
}

static void glb_set_vsync(int on) {
    SDL_GL_SetSwapInterval(on ? 1 : 0);
}

static void glb_set_viewport(int x, int y, int w, int h) {
    glViewport(x, y, w, h);
}

static void glb_set_scissor(int x, int y, int w, int h) {
    glScissor(x, y, w, h);
    glEnable(GL_SCISSOR_TEST);
}

static void glb_set_blend(int type, int src, int dst, int op) {
    if (type == GX_BM_NONE) {
        glDisable(GL_BLEND);
    } else {
        glEnable(GL_BLEND);

        GLenum gl_src, gl_dst;
        switch (src) {
            case GX_BL_ZERO: gl_src = GL_ZERO; break;
            case GX_BL_ONE: gl_src = GL_ONE; break;
            case GX_BL_DSTCLR: gl_src = GL_DST_COLOR; break;
            case GX_BL_INVDSTCLR: gl_src = GL_ONE_MINUS_DST_COLOR; break;
            case GX_BL_SRCALPHA: gl_src = GL_SRC_ALPHA; break;
            case GX_BL_INVSRCALPHA: gl_src = GL_ONE_MINUS_SRC_ALPHA; break;
            case GX_BL_DSTALPHA: gl_src = GL_DST_ALPHA; break;
            case GX_BL_INVDSTALPHA: gl_src = GL_ONE_MINUS_DST_ALPHA; break;
            default: gl_src = GL_ONE; break;
        }
        switch (dst) {
            case GX_BL_ZERO: gl_dst = GL_ZERO; break;
            case GX_BL_ONE: gl_dst = GL_ONE; break;
            case GX_BL_SRCCLR: gl_dst = GL_SRC_COLOR; break;
            case GX_BL_INVSRCCLR: gl_dst = GL_ONE_MINUS_SRC_COLOR; break;
            case GX_BL_SRCALPHA: gl_dst = GL_SRC_ALPHA; break;
            case GX_BL_INVSRCALPHA: gl_dst = GL_ONE_MINUS_SRC_ALPHA; break;
            case GX_BL_DSTALPHA: gl_dst = GL_DST_ALPHA; break;
            case GX_BL_INVDSTALPHA: gl_dst = GL_ONE_MINUS_DST_ALPHA; break;
            default: gl_dst = GL_ZERO; break;
        }

        glBlendFuncSeparate(gl_src, gl_dst, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        if (op == GX_BM_SUBTRACT) {
            my_glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
        } else {
            my_glBlendEquation(GL_FUNC_ADD);
        }
    }
}

static void glb_set_depth(int compare_enable, int func, int update_enable) {
    /* In GL, disabling GL_DEPTH_TEST also disables depth writes, so keep the
     * test enabled and emulate "compare off" with GL_ALWAYS. */
    glEnable(GL_DEPTH_TEST);

    GLenum gl_func;
    switch (compare_enable ? func : GX_ALWAYS) {
        case GX_NEVER: gl_func = GL_NEVER; break;
        case GX_LESS: gl_func = GL_LESS; break;
        case GX_EQUAL: gl_func = GL_EQUAL; break;
        case GX_LEQUAL: gl_func = GL_LEQUAL; break;
        case GX_GREATER: gl_func = GL_GREATER; break;
        case GX_NEQUAL: gl_func = GL_NOTEQUAL; break;
        case GX_GEQUAL: gl_func = GL_GEQUAL; break;
        case GX_ALWAYS: gl_func = GL_ALWAYS; break;
        default: gl_func = GL_LEQUAL; break;
    }
    glDepthFunc(gl_func);
    glDepthMask(update_enable ? GL_TRUE : GL_FALSE);
}

static void glb_set_cull(int mode) {
    switch (mode) {
        case GX_CULL_NONE:
            glDisable(GL_CULL_FACE);
            break;
        case GX_CULL_FRONT:
            glEnable(GL_CULL_FACE);
            glCullFace(GL_FRONT);
            break;
        case GX_CULL_BACK:
            glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);
            break;
        case GX_CULL_ALL:
            glEnable(GL_CULL_FACE);
            glCullFace(GL_FRONT_AND_BACK);
            break;
    }
}

static void glb_clear(const int* rect, uint32_t color, float depth, int force) {
    GLfloat r = ((color >> 24) & 0xFF) / 255.0f;
    GLfloat g = ((color >> 16) & 0xFF) / 255.0f;
    GLfloat b = ((color >> 8) & 0xFF) / 255.0f;
    GLfloat a = (color & 0xFF) / 255.0f;
    GLboolean sc = glIsEnabled(GL_SCISSOR_TEST), cm[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE}, dm = GL_TRUE;
    GLint box[4] = {0, 0, 0, 0};

    glClearColor(r, g, b, a);
    glClearDepth(depth);
    if (force) {   /* EFB copy-clear: colour + depth whatever the masks */
        glGetBooleanv(GL_COLOR_WRITEMASK, cm);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &dm);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
    } else {
        glClearStencil(0);
    }
    if (rect) {
        glGetIntegerv(GL_SCISSOR_BOX, box);
        glEnable(GL_SCISSOR_TEST);
        glScissor(rect[0], rect[1], rect[2], rect[3]);
    } else if (sc) {
        glDisable(GL_SCISSOR_TEST);
    }
    glClear(force ? (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT)
                  : (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT));
    if (rect) {
        glScissor(box[0], box[1], box[2], box[3]);
        if (!sc) glDisable(GL_SCISSOR_TEST);
    } else if (sc) {
        glEnable(GL_SCISSOR_TEST);
    }
    if (force) {
        glColorMask(cm[0], cm[1], cm[2], cm[3]);
        glDepthMask(dm);
    }
}

static GLenum gx_wrap_to_gl(int w) {
    return (w == GX_REPEAT) ? GL_REPEAT : (w == GX_MIRROR) ? GL_MIRRORED_REPEAT : GL_CLAMP_TO_EDGE;
}

static void* glb_tex_create(const uint32_t* rgba, int w, int h, int wrapS, int wrapT, int mipmap) {
    GLuint gl_tex;
    GLint prev = 0;
    /* created on the active unit: put its texture back afterwards (the core
     * may create a texture between binding a stage and drawing) */
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    glGenTextures(1, &gl_tex);
    glBindTexture(GL_TEXTURE_2D, gl_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);

    /* Wrap modes */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, gx_wrap_to_gl(wrapS));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, gx_wrap_to_gl(wrapT));

    /* Filtering */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mipmap ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    if (mipmap) {
        glGenerateMipmap(GL_TEXTURE_2D);
    }
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
    return (void*)(uintptr_t)gl_tex;
}

/* EFB-copy textures: level 0 only, LINEAR, (re)defined on texture unit 0
 * (unit 0 is rebound to its texture afterwards) */
static void glb_tex_image(void** tex, const uint32_t* rgba, int w, int h) {
    GLuint id = (GLuint)(uintptr_t)*tex;
    if (!id) {
        glGenTextures(1, &id);
        *tex = (void*)(uintptr_t)id;
    }
    my_glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    tex_size_note(id, w, h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, g_backend->bound_textures[0]);
}

static void glb_tex_wrap(void* tex, int wrapS, int wrapT) {
    my_glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)(uintptr_t)tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, gx_wrap_to_gl(wrapS));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, gx_wrap_to_gl(wrapT));
    glBindTexture(GL_TEXTURE_2D, g_backend->bound_textures[0]);
}

/* GXInitTexObjLOD: GX filters / LOD clamp / bias / anisotropy applied to the
 * GL texture behind the object.  GX filter enum: 0 NEAR, 1 LINEAR,
 * 2 NEAR_MIP_NEAR, 3 LIN_MIP_NEAR, 4 NEAR_MIP_LIN, 5 LIN_MIP_LIN.
 * maxAniso: 0 = 1x, 1 = 2x, 2 = 4x. */
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif
#ifndef GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif
static void glb_tex_lod(void* handle, int mipmap, int minFilt, int magFilt, float minLod,
                        float maxLod, float lodBias, int maxAniso) {
    static const GLenum min_mip[6] = { GL_NEAREST, GL_LINEAR, GL_NEAREST_MIPMAP_NEAREST,
                                       GL_LINEAR_MIPMAP_NEAREST, GL_NEAREST_MIPMAP_LINEAR,
                                       GL_LINEAR_MIPMAP_LINEAR };
    static const GLenum min_nomip[6] = { GL_NEAREST, GL_LINEAR, GL_NEAREST, GL_LINEAR,
                                         GL_NEAREST, GL_LINEAR };
    static float hw_max_aniso = -1.0f;
    GLuint tex = (GLuint)(uintptr_t)handle;
    GLint prev = 0;
    float aniso;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    mipmap ? min_mip[minFilt] : min_nomip[minFilt]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magFilt == 0 ? GL_NEAREST : GL_LINEAR);
    if (mipmap) {
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_LOD, minLod);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_LOD, maxLod > 0.0f ? maxLod : 1000.0f);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, lodBias);
        if (hw_max_aniso < 0.0f) {
            hw_max_aniso = 0.0f;
            glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &hw_max_aniso);
            while (glGetError() != GL_NO_ERROR) {}
        }
        aniso = (float)(1 << (maxAniso & 3));
        if (hw_max_aniso >= 1.0f) {
            if (aniso > hw_max_aniso) aniso = hw_max_aniso;
            glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, aniso);
        }
    }
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
}

static void glb_tex_destroy(void* handle) {
    GLuint gl_tex = (GLuint)(uintptr_t)handle;
    int i;
    if (!gl_tex) return;
    /* the name may be handed out again: forget it as a binding */
    for (i = 0; i < 8; i++)
        if (g_backend->bound_textures[i] == gl_tex) g_backend->bound_textures[i] = 0;
    glDeleteTextures(1, &gl_tex);
}

static void glb_bind_tex(int stage, void* handle) {
    GLuint gl_tex = (GLuint)(uintptr_t)handle;
    my_glActiveTexture(GL_TEXTURE0 + stage);
    glBindTexture(GL_TEXTURE_2D, gl_tex);
    g_backend->bound_textures[stage] = gl_tex;
}

/* the constants the core built, through the uniform cache */
static void glb_upload(GLBackend* gl, const GxDrawCall* d) {
    const GxDrawConsts* dc = d->dc;
    const GxTevConsts* tc = d->tc;
    int i;

    uc_mat4(gl->u_projection, 1, dc->proj);
    uc_mat4(gl->u_modelview, 1, dc->mv);
    for (i = 0; i < 8; i++)
        uc_mat4(s_u_texmatrix[i], 1, dc->texmtx[i]);

    if (gl->u_mode >= 0) {
        uc_1i(gl->u_mode, dc->mode[0]);
        if (gxfx_glUniform4iv) uc_4iv(gxfx_glUniform4iv, gl->u_acmp, 1, dc->acmp);
        if (dc->mode[0]) {
            uc_mat4(gl->u_nrm, 1, dc->nrm);
            uc_1i(gl->u_tex_valid, dc->tg2[1]);
            if (gl->tev_serial != d->tc_serial && gxfx_glUniform4iv) {
                gl->tev_serial = d->tc_serial;
                uc_4iv(gxfx_glUniform4iv, gl->u_chan, 2, &tc->chan[0][0]);
                uc_4iv(gxfx_glUniform4iv, gl->u_chanfn, 2, &tc->chanfn[0][0]);
                uc_4fv(gl->u_mat, 1, tc->mat);
                uc_4fv(gl->u_amb, 1, tc->amb);
                uc_4fv(gl->u_lpos, 8, &tc->lpos[0][0]);
                uc_4fv(gl->u_ldir, 8, &tc->ldir[0][0]);
                uc_4fv(gl->u_lcos, 8, &tc->lcos[0][0]);
                uc_4fv(gl->u_ldist, 8, &tc->ldist[0][0]);
                uc_4fv(gl->u_lcol, 8, &tc->lcol[0][0]);
                uc_1i(gl->u_tev_n, tc->misc[0]);
                uc_4iv(gxfx_glUniform4iv, gl->u_cin, 8, &tc->cin[0][0]);
                uc_4iv(gxfx_glUniform4iv, gl->u_ain, 8, &tc->ain[0][0]);
                uc_4iv(gxfx_glUniform4iv, gl->u_cop, 8, &tc->cop[0][0]);
                uc_4iv(gxfx_glUniform4iv, gl->u_aop, 8, &tc->aop[0][0]);
                uc_4iv(gxfx_glUniform4iv, gl->u_ord, 8, &tc->ord[0][0]);
                uc_4fv(gl->u_konst, 8, &tc->konst[0][0]);
                uc_4fv(gl->u_reg, 4, &tc->reg[0][0]);
                if (gl->u_ind_on >= 0) {
                    uc_1i(gl->u_ind_on, tc->misc[1]);
                    if (tc->misc[1]) {
                        uc_1i(gl->u_ind_n, tc->misc[2]);
                        uc_4iv(gxfx_glUniform4iv, gl->u_iord, 4, &tc->iord[0][0]);
                        uc_4fv(gl->u_imtx, 6, &tc->imtx[0][0]);
                        uc_4iv(gxfx_glUniform4iv, gl->u_tind, 8, &tc->tind[0][0]);
                    }
                }
            }
        }
    }
    /* texgen sources / post matrices / fog */
    uc_1i(gl->l_nrmmask, dc->tg[2]);
    uc_1i(gl->l_normmask, dc->tg[3]);
    uc_1i(gl->l_postmask, dc->tg2[0]);
    uc_1i(gl->l_q1mask, dc->tg2[2]);
    if (gl->post_serial != d->tc_serial) {
        gl->post_serial = d->tc_serial;
        uc_mat4(gl->l_post, 8, &tc->tg_post[0][0]);
    }
    uc_1i(gl->l_srcsel, dc->tg[1]);
    uc_1i(gl->l_posmask, dc->tg[0]);
    uc_4fv(gl->l_fog, 1, dc->fog);
    uc_4fv(gl->l_fogc, 1, dc->fog_color);
    if (gl->u_ui_tev_loc >= 0) {
        uc_1i(gl->u_ui_tev_loc, dc->mode[1]);
        uc_4fv(gl->u_tev_c0_loc, 1, dc->tev_c0);
        uc_4fv(gl->u_tev_c1_loc, 1, dc->tev_c1);
    }
    if (gl->u_has_tex_loc >= 0)
        uc_1i(gl->u_has_tex_loc, dc->mode[2]);
    if (gl->u_uvclamp_loc >= 0) {
        uc_1i(gl->u_uvclamp_loc, dc->mode[3]);
        if (glUniform2f) uc_2f(gl->u_efb_scale_loc, dc->efb_scale[0], dc->efb_scale[1]);
    }
}

/* static index buffer that maps quad vertices 0123 -> triangles 012 023 */
static GLuint s_quad_ebo;

static GLenum gxd_to_gl(int prim) {
    switch (prim) {
        case GXD_TRISTRIP: return GL_TRIANGLE_STRIP;
        case GXD_TRIFAN:   return GL_TRIANGLE_FAN;
        case GXD_POINTS:   return GL_POINTS;
        default:           return GL_TRIANGLES;
    }
}

static void glb_draw(const GxDrawCall* d) {
    GLBackend* gl = g_backend;
    int base, n = d->nverts;
    GLboolean cull_was_on = GL_FALSE;
    if (!gl) return;
    glb_upload(gl, d);
    glBindVertexArray(gl->vao);
    my_glBindBuffer(GL_ARRAY_BUFFER, gl->vbo);
    if (n > 0) {
        base = ring_alloc(gl, n);
        ring_write(base, d->verts, n);
        if (d->rects) ring_write_rect(gl, base, d->rects, n);
        if (d->no_cull) {
            cull_was_on = glIsEnabled(GL_CULL_FACE);
            glDisable(GL_CULL_FACE);
        }
        /* GL_DEPTH_CLAMP also disables near-plane clipping: ortho only */
        if (d->depth_clamp)
            glEnable(GL_DEPTH_CLAMP);
        else
            glDisable(GL_DEPTH_CLAMP);
        if (d->prim == GXD_QUADS) {
            int quads = n / 4;
            if (!s_quad_ebo) {
                int maxq = GX_MAX_BATCH_VERTS / 4;
                GLuint* idx = malloc((size_t)maxq * 6 * sizeof(GLuint));
                for (int q = 0; q < maxq; q++) {
                    idx[q * 6 + 0] = q * 4 + 0;
                    idx[q * 6 + 1] = q * 4 + 1;
                    idx[q * 6 + 2] = q * 4 + 2;
                    idx[q * 6 + 3] = q * 4 + 0;
                    idx[q * 6 + 4] = q * 4 + 2;
                    idx[q * 6 + 5] = q * 4 + 3;
                }
                my_glGenBuffers(1, &s_quad_ebo);
                my_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_quad_ebo);
                my_glBufferData(GL_ELEMENT_ARRAY_BUFFER, (size_t)maxq * 6 * sizeof(GLuint), idx, GL_STATIC_DRAW);
                free(idx);
            } else {
                my_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_quad_ebo);
            }
            if (s_glDrawElementsBaseVertex)
                s_glDrawElementsBaseVertex(GL_TRIANGLES, quads * 6, GL_UNSIGNED_INT, 0, base);
            else
                glDrawElements(GL_TRIANGLES, quads * 6, GL_UNSIGNED_INT, 0);
        } else {
            my_glDrawArrays(gxd_to_gl(d->prim), base, n);
        }
        if (d->no_cull && cull_was_on)
            glEnable(GL_CULL_FACE);
    }
    glBindVertexArray(0);
}

static int glb_read_pixels(int x, int y, int w, int h, int depth, void* out) {
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    if (depth)
        glReadPixels(x, y, w, h, GL_DEPTH_COMPONENT, GL_FLOAT, out);
    else
        glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, out);
    return 1;
}

static int glb_read_frame(unsigned char** rgb, int* pw, int* ph) {
    static unsigned char *buf, *flip;
    static size_t buf_size;
    SDL_Window* win = SDL_GL_GetCurrentWindow();
    int w = 0, h = 0, y;
    size_t row;
    if (!win) return 0;
    SDL_GL_GetDrawableSize(win, &w, &h);
    if (w <= 0 || h <= 0) return 0;
    row = (size_t)w * 3;
    if (!buf || buf_size < row * h) {   /* window can be resized */
        free(buf); free(flip);
        buf_size = row * h;
        buf = malloc(buf_size);
        flip = malloc(buf_size);
        if (!buf || !flip) { free(buf); free(flip); buf = flip = NULL; return 0; }
    }
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, buf);
    for (y = 0; y < h; y++)               /* GL rows are bottom-up */
        memcpy(flip + (size_t)y * row, buf + (size_t)(h - 1 - y) * row, row);
    *rgb = flip; *pw = w; *ph = h;
    return 1;
}

/* ---- PC port settings overlay (F1): keep the finished game frame ----
 * pc_frame_capture() copies the back buffer (the frame the game just drew,
 * not yet presented) into a texture FBO; pc_frame_restore() draws it back to
 * the back buffer, scaled to the current drawable (the window may have been
 * resized from the overlay).  Blits ignore everything but the scissor test. */
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
typedef void (APIENTRY *PFN_pc_GenFramebuffers)(GLsizei, GLuint*);
typedef void (APIENTRY *PFN_pc_BindFramebuffer)(GLenum, GLuint);
typedef void (APIENTRY *PFN_pc_FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum (APIENTRY *PFN_pc_CheckFramebufferStatus)(GLenum);
typedef void (APIENTRY *PFN_pc_BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint,
                                                GLint, GLint, GLbitfield, GLenum);
static PFN_pc_GenFramebuffers p_genfb;
static PFN_pc_BindFramebuffer p_bindfb;
static PFN_pc_FramebufferTexture2D p_fbtex;
static PFN_pc_CheckFramebufferStatus p_fbstatus;
static PFN_pc_BlitFramebuffer p_blit;
static GLuint s_cap_fbo, s_cap_tex;
static int s_cap_w, s_cap_h, s_cap_ok;

static void pc_blit(GLuint rd, GLuint dr, int sw, int sh, int dw, int dh) {
    int sc = glIsEnabled(GL_SCISSOR_TEST);
    if (sc) glDisable(GL_SCISSOR_TEST);
    p_bindfb(GL_READ_FRAMEBUFFER, rd);
    p_bindfb(GL_DRAW_FRAMEBUFFER, dr);
    p_blit(0, 0, sw, sh, 0, 0, dw, dh, GL_COLOR_BUFFER_BIT,
           (sw == dw && sh == dh) ? GL_NEAREST : GL_LINEAR);
    p_bindfb(GL_READ_FRAMEBUFFER, 0);
    p_bindfb(GL_DRAW_FRAMEBUFFER, 0);
    if (sc) glEnable(GL_SCISSOR_TEST);
}

static int glb_capture(void) {
    GLBackend* gl = g_backend;
    GLint prev_tex = 0;
    if (!gl) return 0;
    if (!p_blit) {
        p_genfb = (PFN_pc_GenFramebuffers)SDL_GL_GetProcAddress("glGenFramebuffers");
        p_bindfb = (PFN_pc_BindFramebuffer)SDL_GL_GetProcAddress("glBindFramebuffer");
        p_fbtex = (PFN_pc_FramebufferTexture2D)SDL_GL_GetProcAddress("glFramebufferTexture2D");
        p_fbstatus = (PFN_pc_CheckFramebufferStatus)SDL_GL_GetProcAddress("glCheckFramebufferStatus");
        p_blit = (PFN_pc_BlitFramebuffer)SDL_GL_GetProcAddress("glBlitFramebuffer");
        if (!p_genfb || !p_bindfb || !p_fbtex || !p_fbstatus || !p_blit) { p_blit = NULL; return 0; }
    }
    if (!s_cap_fbo || s_cap_w != gl->width || s_cap_h != gl->height) {
        if (!s_cap_fbo) p_genfb(1, &s_cap_fbo);
        if (!s_cap_tex) glGenTextures(1, &s_cap_tex);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
        glBindTexture(GL_TEXTURE_2D, s_cap_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, gl->width, gl->height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);
        p_bindfb(GL_DRAW_FRAMEBUFFER, s_cap_fbo);
        p_fbtex(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_cap_tex, 0);
        s_cap_ok = p_fbstatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        p_bindfb(GL_DRAW_FRAMEBUFFER, 0);
        s_cap_w = gl->width;
        s_cap_h = gl->height;
    }
    if (!s_cap_ok) return 0;
    pc_blit(0, s_cap_fbo, s_cap_w, s_cap_h, s_cap_w, s_cap_h);
    return 1;
}

/* Blitting into the (multisampled) default framebuffer is not allowed, so
 * the restore draws the capture as a full-screen triangle with a tiny
 * program, saving and restoring every bit of GL state it touches. */
static GLuint s_cap_prog, s_cap_vao;
static GLint s_cap_dim_loc = -1;
static int s_cap_prog_failed;

static int pc_cap_program(void) {
    static const char* vs =
        "#version 330 core\n"
        "out vec2 uv;\n"
        "void main() {\n"
        "    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
        "    uv = p;\n"
        "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
        "}\n";
    static const char* fs =
        "#version 330 core\n"
        "in vec2 uv;\n"
        "out vec4 o;\n"
        "uniform sampler2D t;\n"
        "uniform float dim;\n"
        "void main() { o = vec4(texture(t, uv).rgb * dim, 1.0); }\n";
    GLuint v, f;
    GLint ok = 0;
    if (s_cap_prog) return 1;
    if (s_cap_prog_failed) return 0;
    v = compile_shader(GL_VERTEX_SHADER, vs);
    f = compile_shader(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) { s_cap_prog_failed = 1; return 0; }
    s_cap_prog = glCreateProgram();
    glAttachShader(s_cap_prog, v);
    glAttachShader(s_cap_prog, f);
    glLinkProgram(s_cap_prog);
    glDeleteShader(v);
    glDeleteShader(f);
    glGetProgramiv(s_cap_prog, GL_LINK_STATUS, &ok);
    if (!ok) { s_cap_prog = 0; s_cap_prog_failed = 1; return 0; }
    s_cap_dim_loc = glGetUniformLocation(s_cap_prog, "dim");
    glGenVertexArrays(1, &s_cap_vao);
    return 1;
}

static void glb_restore(float brightness) {
    GLBackend* gl = g_backend;
    GLint prog, vao, active, tex0, vp[4], dfb;
    GLboolean blend, depth, cull, scis, stencil, dmask, cmask[4];
    if (!gl || !s_cap_ok || !p_blit || !pc_cap_program()) return;

    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    my_glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex0);
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &dfb);
    blend = glIsEnabled(GL_BLEND);
    depth = glIsEnabled(GL_DEPTH_TEST);
    cull = glIsEnabled(GL_CULL_FACE);
    scis = glIsEnabled(GL_SCISSOR_TEST);
    stencil = glIsEnabled(GL_STENCIL_TEST);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &dmask);
    glGetBooleanv(GL_COLOR_WRITEMASK, cmask);

    p_bindfb(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glViewport(0, 0, gl->width, gl->height);
    glUseProgram(s_cap_prog);
    glUniform1i(glGetUniformLocation(s_cap_prog, "t"), 0);
    {
        typedef void (APIENTRY *PFN_u1f)(GLint, GLfloat);
        static PFN_u1f p_u1f;
        if (!p_u1f) p_u1f = (PFN_u1f)SDL_GL_GetProcAddress("glUniform1f");
        if (p_u1f) p_u1f(s_cap_dim_loc, brightness);
    }
    glBindVertexArray(s_cap_vao);
    glBindTexture(GL_TEXTURE_2D, s_cap_tex);
    my_glDrawArrays(GL_TRIANGLES, 0, 3);

    glBindTexture(GL_TEXTURE_2D, (GLuint)tex0);
    my_glActiveTexture((GLenum)active);
    glBindVertexArray((GLuint)vao);
    glUseProgram((GLuint)prog);
    p_bindfb(GL_DRAW_FRAMEBUFFER, (GLuint)dfb);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    if (blend) glEnable(GL_BLEND);
    if (depth) glEnable(GL_DEPTH_TEST);
    if (cull) glEnable(GL_CULL_FACE);
    if (scis) glEnable(GL_SCISSOR_TEST);
    if (stencil) glEnable(GL_STENCIL_TEST);
    glDepthMask(dmask);
    glColorMask(cmask[0], cmask[1], cmask[2], cmask[3]);
}

/* GXCopyTex on the GPU (gx_gpu.h read_pixels_box / copy_tex): blit (= MSAA
 * resolve) the default framebuffer into a texture, area-resample the rect to
 * dw x dh with a tiny program (the exact integer box mean of GXCopyTex's CPU
 * loop) into an RGBA8UI target and, for copy_tex, the texels converted
 * through the copy format into the game texture (second colour output).
 * read_pixels_box reads the result back synchronously; copy_tex reads it into
 * a PBO with a fence and never waits. */
#ifndef GL_RGBA8UI
#define GL_RGBA8UI 0x8D7C
#endif
#ifndef GL_RGBA_INTEGER
#define GL_RGBA_INTEGER 0x8D99
#endif
#ifndef GL_READ_FRAMEBUFFER_BINDING
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#endif
#ifndef GL_COLOR_ATTACHMENT1
#define GL_COLOR_ATTACHMENT1 0x8CE1
#endif
#ifndef GL_PIXEL_PACK_BUFFER
#define GL_PIXEL_PACK_BUFFER 0x88EB
#endif
#ifndef GL_STREAM_READ
#define GL_STREAM_READ 0x88E1
#endif
#ifndef GL_MAP_READ_BIT
#define GL_MAP_READ_BIT 0x0001
#endif
#ifndef GL_SYNC_GPU_COMMANDS_COMPLETE
#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#endif
#ifndef GL_SYNC_FLUSH_COMMANDS_BIT
#define GL_SYNC_FLUSH_COMMANDS_BIT 0x00000001
#endif
#ifndef GL_ALREADY_SIGNALED
#define GL_ALREADY_SIGNALED 0x911A
#endif
#ifndef GL_CONDITION_SATISFIED
#define GL_CONDITION_SATISFIED 0x911C
#endif
#ifndef GL_WAIT_FAILED
#define GL_WAIT_FAILED 0x911D
#endif
typedef void (APIENTRY *PFN_pc_DrawBuffers)(GLsizei, const GLenum*);
typedef GLboolean (APIENTRY *PFN_pc_UnmapBuffer)(GLenum);
static PFN_pc_DrawBuffers p_drawbufs;
static PFN_pc_UnmapBuffer p_unmapbuf;
static RING_PFNMAPBUFFERRANGE p_mapbufrange;
static GLuint s_box_prog, s_box_src_fbo, s_box_src_tex, s_box_dst_fbo, s_box_dst_tex;
static GLint s_box_loc0 = -1, s_box_loc1 = -1, s_box_loccf = -1;
static int s_box_src_w, s_box_src_h, s_box_dst_w, s_box_dst_h, s_box_failed;

static int pc_box_program(void) {
    static const char* vs =
        "#version 330 core\n"
        "void main() {\n"
        "    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
        "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
        "}\n";
    static const char* fs =
        "#version 330 core\n"
        "layout(location = 0) out uvec4 o;\n"
        "layout(location = 1) out vec4 c;\n"
        "uniform sampler2D t;\n"
        "uniform ivec4 b0;\n"   /* x0, top GL row of the rect, w, h */
        "uniform ivec4 b1;\n"   /* dw, dh, target w, target h */
        "uniform int cf;\n"     /* copy format (copy_tex) */
        "uvec4 conv(int cf, uvec4 p) {\n"   /* render_d3d12.c copy_conv */
        "    uint r = p.x, g = p.y, b = p.z, a = 255u, v;\n"
        "    uint l = min(255u, (66u * r + 129u * g + 25u * b + 128u) / 256u + 16u);\n"
        "    if (cf == 0x00) { v = (l >> 4) * 17u; return uvec4(v, v, v, v); }\n"
        "    if (cf == 0x01) return uvec4(l, l, l, l);\n"
        "    if (cf == 0x02) { v = (l >> 4) * 17u; return uvec4(v, v, v, (a >> 4) * 17u); }\n"
        "    if (cf == 0x03) return uvec4(l, l, l, a);\n"
        "    if (cf == 0x20) { v = (r >> 4) * 17u; return uvec4(v, v, v, v); }\n"
        "    if (cf == 0x28) return uvec4(r, r, r, r);\n"
        "    if (cf == 0x29) return uvec4(g, g, g, g);\n"
        "    if (cf == 0x2A) return uvec4(b, b, b, b);\n"
        "    if (cf == 0x27) return uvec4(a, a, a, a);\n"
        "    if (cf == 0x22) { v = (r >> 4) * 17u; return uvec4(v, v, v, (a >> 4) * 17u); }\n"
        "    if (cf == 0x23) return uvec4(r, r, r, a);\n"
        "    if (cf == 0x2B) return uvec4(r, r, r, g);\n"
        "    if (cf == 0x2C) return uvec4(g, g, g, b);\n"
        "    if (cf == 0x04) {\n"
        "        uint r5 = r >> 3, g6 = g >> 2, b5 = b >> 3;\n"
        "        return uvec4((r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2), 255u);\n"
        "    }\n"
        "    if (cf == 0x05) {\n"
        "        uint r5 = r >> 3, g5 = g >> 3, b5 = b >> 3;\n"
        "        return uvec4((r5 << 3) | (r5 >> 2), (g5 << 3) | (g5 >> 2), (b5 << 3) | (b5 >> 2), 255u);\n"
        "    }\n"
        "    return uvec4(r, g, b, a);\n"
        "}\n"
        "void main() {\n"
        "    int ox = int(gl_FragCoord.x), oy = int(gl_FragCoord.y);\n"
        "    int W = b0.z, H = b0.w, dw = b1.x, dh = b1.y;\n"
        "    int xa = ox * W / dw, xb = (ox + 1) * W / dw;\n"
        "    int ya = oy * H / dh, yb = (oy + 1) * H / dh;\n"
        "    uvec4 acc = uvec4(0u); uint n = 0u;\n"
        "    if (xb <= xa) xb = xa + 1;\n"
        "    if (yb <= ya) yb = ya + 1;\n"
        "    xb = min(xb, W); yb = min(yb, H);\n"
        "    for (int yy = ya; yy < yb; yy++)\n"
        "    for (int xx = xa; xx < xb; xx++) {\n"
        "        int sx = b0.x + xx, sy = b0.y - yy;\n"
        "        if (sx >= 0 && sy >= 0 && sx < b1.z && sy < b1.w)\n"
        "            acc += uvec4(round(texelFetch(t, ivec2(sx, sy), 0) * 255.0));\n"
        "        n++;\n"
        "    }\n"
        "    o = n > 0u ? acc / n : uvec4(0u);\n"
        "    c = vec4(conv(cf, o)) / 255.0;\n"
        "}\n";
    GLuint v, f;
    GLint ok = 0;
    if (s_box_prog) return 1;
    if (s_box_failed || !pc_cap_program()) return 0;   /* its VAO */
    p_drawbufs = (PFN_pc_DrawBuffers)SDL_GL_GetProcAddress("glDrawBuffers");
    p_unmapbuf = (PFN_pc_UnmapBuffer)SDL_GL_GetProcAddress("glUnmapBuffer");
    p_mapbufrange = (RING_PFNMAPBUFFERRANGE)SDL_GL_GetProcAddress("glMapBufferRange");
    if (!s_glFenceSync) {
        s_glFenceSync = (RING_PFNFENCESYNC)SDL_GL_GetProcAddress("glFenceSync");
        s_glClientWaitSync = (RING_PFNCLIENTWAITSYNC)SDL_GL_GetProcAddress("glClientWaitSync");
        s_glDeleteSync = (RING_PFNDELETESYNC)SDL_GL_GetProcAddress("glDeleteSync");
    }
    v = compile_shader(GL_VERTEX_SHADER, vs);
    f = compile_shader(GL_FRAGMENT_SHADER, fs);
    if (!v || !f || !gxfx_glUniform4iv || !p_drawbufs) { s_box_failed = 1; return 0; }
    s_box_prog = glCreateProgram();
    glAttachShader(s_box_prog, v);
    glAttachShader(s_box_prog, f);
    glLinkProgram(s_box_prog);
    glDeleteShader(v);
    glDeleteShader(f);
    glGetProgramiv(s_box_prog, GL_LINK_STATUS, &ok);
    if (!ok) { s_box_prog = 0; s_box_failed = 1; return 0; }
    s_box_loc0 = glGetUniformLocation(s_box_prog, "b0");
    s_box_loc1 = glGetUniformLocation(s_box_prog, "b1");
    s_box_loccf = glGetUniformLocation(s_box_prog, "cf");
    return 1;
}

static int pc_box_target(GLuint* fbo, GLuint* tex, int* cw, int* ch, int w, int h, GLint ifmt,
                         GLenum fmt) {
    if (*fbo && *cw == w && *ch == h) return 1;
    if (!*fbo) p_genfb(1, fbo);
    if (!*tex) glGenTextures(1, tex);
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexImage2D(GL_TEXTURE_2D, 0, ifmt, w, h, 0, fmt, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    p_bindfb(GL_DRAW_FRAMEBUFFER, *fbo);
    p_fbtex(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
    if (p_fbstatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { *cw = *ch = 0; return 0; }
    *cw = w; *ch = h;
    return 1;
}

/* GL state the box passes touch.  No glGet* / glGetError here: with a
 * threaded driver each query drains the driver queue.  Enables, viewport,
 * colour mask, texture unit and bindings come from the st_* mirror above;
 * the program is the backend's (bound at frame start), no VAO is bound
 * between draws, and the default framebuffer is bound outside these passes. */
typedef struct {
    int unit;
    GLuint tex0;
    GLint vp[4];
    GLboolean blend, depth, cull, scis, stencil, cmask[4];
} BoxSaved;

static int box_begin(BoxSaved* sv, int dw, int dh) {
    GLBackend* gl = g_backend;
    if (!gl || dw <= 0 || dh <= 0 || dw > 4096 || dh > 4096) return 0;
    if (!p_blit) {   /* same entry points as the F1 capture */
        p_genfb = (PFN_pc_GenFramebuffers)SDL_GL_GetProcAddress("glGenFramebuffers");
        p_bindfb = (PFN_pc_BindFramebuffer)SDL_GL_GetProcAddress("glBindFramebuffer");
        p_fbtex = (PFN_pc_FramebufferTexture2D)SDL_GL_GetProcAddress("glFramebufferTexture2D");
        p_fbstatus = (PFN_pc_CheckFramebufferStatus)SDL_GL_GetProcAddress("glCheckFramebufferStatus");
        p_blit = (PFN_pc_BlitFramebuffer)SDL_GL_GetProcAddress("glBlitFramebuffer");
        if (!p_genfb || !p_bindfb || !p_fbtex || !p_fbstatus || !p_blit) { p_blit = NULL; return 0; }
    }
    if (!pc_box_program()) return 0;
    sv->unit = s_st_unit;
    my_glActiveTexture(GL_TEXTURE0);
    sv->tex0 = s_st_tex[0];
    glGetIntegerv(GL_VIEWPORT, sv->vp);          /* mirrored */
    sv->blend = glIsEnabled(GL_BLEND);           /* mirrored */
    sv->depth = glIsEnabled(GL_DEPTH_TEST);
    sv->cull = glIsEnabled(GL_CULL_FACE);
    sv->scis = glIsEnabled(GL_SCISSOR_TEST);
    sv->stencil = glIsEnabled(GL_STENCIL_TEST);
    glGetBooleanv(GL_COLOR_WRITEMASK, sv->cmask); /* mirrored */
    return 1;
}

static void box_end(const BoxSaved* sv) {
    glBindTexture(GL_TEXTURE_2D, sv->tex0);
    my_glActiveTexture((GLenum)(GL_TEXTURE0 + sv->unit));
    glBindVertexArray(0);
    glUseProgram(g_backend->shader_program);
    p_bindfb(GL_READ_FRAMEBUFFER, 0);
    p_bindfb(GL_DRAW_FRAMEBUFFER, 0);
    glViewport(sv->vp[0], sv->vp[1], sv->vp[2], sv->vp[3]);
    if (sv->blend) glEnable(GL_BLEND);
    if (sv->depth) glEnable(GL_DEPTH_TEST);
    if (sv->cull) glEnable(GL_CULL_FACE);
    if (sv->scis) glEnable(GL_SCISSOR_TEST);
    if (sv->stencil) glEnable(GL_STENCIL_TEST);
    glColorMask(sv->cmask[0], sv->cmask[1], sv->cmask[2], sv->cmask[3]);
}

/* resolve the frame into s_box_src_tex and run the box program into
 * s_box_dst_fbo (attachment 0, plus attachment 1 when bound by the caller) */
static int box_run(int x, int y, int w, int h, int dw, int dh, int cf) {
    GLBackend* gl = g_backend;
    GLint b0[4], b1[4];
    if (!pc_box_target(&s_box_src_fbo, &s_box_src_tex, &s_box_src_w, &s_box_src_h,
                       gl->width, gl->height, GL_RGBA8, GL_RGBA) ||
        !pc_box_target(&s_box_dst_fbo, &s_box_dst_tex, &s_box_dst_w, &s_box_dst_h,
                       dw > s_box_dst_w ? dw : s_box_dst_w, dh > s_box_dst_h ? dh : s_box_dst_h,
                       GL_RGBA8UI, GL_RGBA_INTEGER))
        return 0;
    glDisable(GL_SCISSOR_TEST);
    p_bindfb(GL_READ_FRAMEBUFFER, 0);
    p_bindfb(GL_DRAW_FRAMEBUFFER, s_box_src_fbo);
    p_blit(0, 0, gl->width, gl->height, 0, 0, gl->width, gl->height, GL_COLOR_BUFFER_BIT,
           GL_NEAREST);
    p_bindfb(GL_DRAW_FRAMEBUFFER, s_box_dst_fbo);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, dw, dh);
    glUseProgram(s_box_prog);
    glUniform1i(glGetUniformLocation(s_box_prog, "t"), 0);
    glUniform1i(s_box_loccf, cf);
    b0[0] = x; b0[1] = y + h - 1; b0[2] = w; b0[3] = h;   /* GL row of rect row yy = b0.y - yy */
    b1[0] = dw; b1[1] = dh; b1[2] = gl->width; b1[3] = gl->height;
    gxfx_glUniform4iv(s_box_loc0, 1, b0);
    gxfx_glUniform4iv(s_box_loc1, 1, b1);
    glBindVertexArray(s_cap_vao);
    glBindTexture(GL_TEXTURE_2D, s_box_src_tex);
    my_glDrawArrays(GL_TRIANGLES, 0, 3);
    return 1;
}

static int glb_read_pixels_box(int x, int y, int w, int h, int dw, int dh, void* out) {
    BoxSaved sv;
    int ok = 0;
    if (w <= 0 || h <= 0 || !box_begin(&sv, dw, dh)) return 0;
    while (glGetError() != GL_NO_ERROR) {}   /* synchronous path: queries are fine */
    if (box_run(x, y, w, h, dw, dh, 0)) {
        /* output row oy is GL row oy of the target: rows come back top-down */
        p_bindfb(GL_READ_FRAMEBUFFER, s_box_dst_fbo);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, dw, dh, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, out);
        ok = glGetError() == GL_NO_ERROR;
    }
    box_end(&sv);
    if (!ok) s_box_failed = 1;   /* don't retry a broken path every copy */
    return ok;
}

/* async copies: PBO + fence per ticket */
#define CT_SLOTS 128
static struct {
    GLuint pbo;
    size_t size;
    GLsync fence;
    uint64_t ticket;
    int dw, dh;
} s_ct[CT_SLOTS];
static uint64_t s_ct_next = 1;
static int s_ct_checked;

/* sizes of the textures copy_tex renders into (glGetTexLevelParameteriv
 * would sync); glb_tex_image / glDeleteTextures keep it current */
static struct { GLuint id; int w, h; } s_ctsz[128];
static void tex_size_get(GLuint id, GLint* w, GLint* h) {
    int i;
    *w = *h = -1;
    for (i = 0; i < 128; i++)
        if (s_ctsz[i].id == id) { *w = s_ctsz[i].w; *h = s_ctsz[i].h; return; }
}
/* glb_tex_image / delete: update the size of a texture we track */
static void tex_size_note(GLuint id, int w, int h) {
    int i;
    for (i = 0; i < 128; i++)
        if (s_ctsz[i].id == id) {
            if (w < 0) s_ctsz[i].id = 0;
            else { s_ctsz[i].w = w; s_ctsz[i].h = h; }
            return;
        }
}
static void tex_size_set(GLuint id, int w, int h) {
    int i, fr = -1;
    for (i = 0; i < 128; i++) {
        if (s_ctsz[i].id == id) { s_ctsz[i].w = w; s_ctsz[i].h = h; return; }
        if (!s_ctsz[i].id && fr < 0) fr = i;
    }
    if (fr < 0) fr = (int)(id % 128);   /* full: overwrite (a miss just re-specifies) */
    s_ctsz[fr].id = id; s_ctsz[fr].w = w; s_ctsz[fr].h = h;
}

static int ct_wait(GLsync f, int wait) {
    GLenum r = s_glClientWaitSync(f, wait ? GL_SYNC_FLUSH_COMMANDS_BIT : 0, 0);
    while (wait && r == GL_TIMEOUT_EXPIRED)
        r = s_glClientWaitSync(f, GL_SYNC_FLUSH_COMMANDS_BIT, 100000000ull);
    return r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED || r == GL_WAIT_FAILED;
}

static uint64_t glb_copy_tex(void** tex, int x, int y, int w, int h, int dw, int dh, int cf) {
    static const GLenum both[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
    static const GLenum one[1] = { GL_COLOR_ATTACHMENT0 };
    BoxSaved sv;
    GLuint id = (GLuint)(uintptr_t)*tex;
    GLint tw = 0, th = 0;
    uint64_t ticket = 0;
    size_t raw = (size_t)dw * dh * 4;
    int slot;
    int norb = (cf & GXCOPY_NO_READBACK) != 0;   /* HFR replay: texture only */
    cf &= 0xFFFF;
    if (w <= 0 || h <= 0 || !box_begin(&sv, dw, dh)) return 0;
    if (!p_mapbufrange || !p_unmapbuf || !s_glFenceSync || !s_glClientWaitSync || !s_glDeleteSync) {
        box_end(&sv);
        return 0;
    }
    slot = -1;
    if (!norb) {   /* a free slot (collected / discarded: no fence left).  No sync
         * queries here: GL orders a new glReadPixels into the PBO after any
         * earlier one, and only mapping needs the fence. */
        int i;
        slot = -1;
        for (i = 0; i < CT_SLOTS; i++)
            if (!s_ct[i].ticket && !s_ct[i].fence) { slot = i; break; }
        if (slot < 0) slot = (int)(s_ct_next % CT_SLOTS);   /* all in flight */
        if (s_ct[slot].fence) {
            ct_wait(s_ct[slot].fence, 1);
            s_glDeleteSync(s_ct[slot].fence);
            s_ct[slot].fence = NULL;
        }
        s_ct[slot].ticket = 0;
    }
    /* the game texture: RGBA8 dw x dh, LINEAR like glb_tex_image */
    if (!id) { glGenTextures(1, &id); *tex = (void*)(uintptr_t)id; }
    glBindTexture(GL_TEXTURE_2D, id);
    tex_size_get(id, &tw, &th);
    if (tw != dw || th != dh) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, dw, dh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        tex_size_set(id, dw, dh);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);
    /* make sure the box target exists before attaching the second output */
    if (pc_box_target(&s_box_dst_fbo, &s_box_dst_tex, &s_box_dst_w, &s_box_dst_h,
                      dw > s_box_dst_w ? dw : s_box_dst_w, dh > s_box_dst_h ? dh : s_box_dst_h,
                      GL_RGBA8UI, GL_RGBA_INTEGER)) {
        p_bindfb(GL_DRAW_FRAMEBUFFER, s_box_dst_fbo);
        p_fbtex(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, id, 0);
        p_drawbufs(2, both);
        if (!s_ct_checked)   /* RGBA8 + RGBA8UI: checked once (a query syncs) */
            s_ct_checked = p_fbstatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE ? 1 : -1;
        if (s_ct_checked > 0 && norb) {
            if (box_run(x, y, w, h, dw, dh, cf)) ticket = 1;
        } else if (s_ct_checked > 0 && box_run(x, y, w, h, dw, dh, cf)) {
            if (!s_ct[slot].pbo) my_glGenBuffers(1, &s_ct[slot].pbo);
            my_glBindBuffer(GL_PIXEL_PACK_BUFFER, s_ct[slot].pbo);
            if (s_ct[slot].size < raw * 2) {
                my_glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)(raw * 2), NULL, GL_STREAM_READ);
                s_ct[slot].size = raw * 2;
            }
            p_bindfb(GL_READ_FRAMEBUFFER, s_box_dst_fbo);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadBuffer(GL_COLOR_ATTACHMENT0);
            glReadPixels(0, 0, dw, dh, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, (void*)0);
            glReadBuffer(GL_COLOR_ATTACHMENT1);
            glReadPixels(0, 0, dw, dh, GL_RGBA, GL_UNSIGNED_BYTE, (void*)(uintptr_t)raw);
            glReadBuffer(GL_COLOR_ATTACHMENT0);
            my_glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            s_ct[slot].fence = s_glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            if (s_ct[slot].fence) {
                ticket = (s_ct_next++ << 8) | (uint64_t)slot;
                s_ct[slot].ticket = ticket;
                s_ct[slot].dw = dw; s_ct[slot].dh = dh;
            }
        }
        p_bindfb(GL_DRAW_FRAMEBUFFER, s_box_dst_fbo);
        p_fbtex(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, 0, 0);
        p_drawbufs(1, one);
    }
    box_end(&sv);
    if (!ticket) s_box_failed = 1;
    return ticket;
}

/* NVX_gpu_memory_info: whole-GPU usage (total - available), 0 elsewhere */
static unsigned long long glb_gpu_mem(void) {
    static int has = -1;
    GLint total = 0, avail = 0;
    if (has < 0) has = SDL_GL_ExtensionSupported("GL_NVX_gpu_memory_info") ? 1 : 0;
    if (!has) return 0;
    glGetIntegerv(0x9048, &total);   /* TOTAL_AVAILABLE_MEMORY_NVX, KB */
    glGetIntegerv(0x9049, &avail);   /* CURRENT_AVAILABLE_VIDMEM_NVX, KB */
    return total > avail ? (unsigned long long)(total - avail) * 1024ull : 0;
}

static int glb_copy_tex_result(uint64_t ticket, void* raw, void* conv, int wait) {
    int slot = (int)(ticket & 0xFF);
    size_t n;
    const uint8_t* map;
    if (!ticket || slot >= CT_SLOTS || s_ct[slot].ticket != ticket || !s_ct[slot].fence) return -1;
    if (wait < 0) {   /* discard: deleting an unsignalled fence is fine (no query) */
        s_glDeleteSync(s_ct[slot].fence);
        s_ct[slot].fence = NULL;
        s_ct[slot].ticket = 0;
        return 1;
    }
    if (!ct_wait(s_ct[slot].fence, wait)) return 0;
    n = (size_t)s_ct[slot].dw * s_ct[slot].dh * 4;
    my_glBindBuffer(GL_PIXEL_PACK_BUFFER, s_ct[slot].pbo);
    map = (const uint8_t*)p_mapbufrange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)(n * 2), GL_MAP_READ_BIT);
    if (map) {
        if (raw) memcpy(raw, map, n);
        if (conv) memcpy(conv, map + n, n);
        p_unmapbuf(GL_PIXEL_PACK_BUFFER);
    }
    my_glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    s_glDeleteSync(s_ct[slot].fence);
    s_ct[slot].fence = NULL;
    s_ct[slot].ticket = 0;
    return map ? 1 : -1;
}

const GxGpu gx_gpu_gl = {
    .name = "OpenGL 3.3 Core",
    .init = glb_init,
    .shutdown = glb_shutdown,
    .begin_frame = glb_begin_frame,
    .resize = glb_resize,
    .present = glb_present,
    .set_vsync = glb_set_vsync,
    .set_viewport = glb_set_viewport,
    .set_scissor = glb_set_scissor,
    .set_blend = glb_set_blend,
    .set_depth = glb_set_depth,
    .set_cull = glb_set_cull,
    .clear = glb_clear,
    .tex_create = glb_tex_create,
    .tex_image = glb_tex_image,
    .tex_wrap = glb_tex_wrap,
    .tex_lod = glb_tex_lod,
    .tex_destroy = glb_tex_destroy,
    .bind_tex = glb_bind_tex,
    .draw = glb_draw,
    .read_pixels = glb_read_pixels,
    .read_pixels_box = glb_read_pixels_box,
    .copy_tex = glb_copy_tex,
    .copy_tex_result = glb_copy_tex_result,
    .gpu_mem = glb_gpu_mem,
    .read_frame = glb_read_frame,
    .capture = glb_capture,
    .restore = glb_restore,
};
