// ravengltrace.c - GL call tracer for reverse-engineering the engine's frame (FBOs, programs, matrices).
// RAVENGFX_GLTRACE=<secs>  : at the first present after <secs>, log every traced GL call of one frame
//                            to stderr ("[gltrace] ..."). Function pointers are found by scanning the
//                            game's writable memory for the real GL entry points and replacing them.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>

typedef unsigned GLenum, GLuint, GLbitfield; typedef int GLint, GLsizei; typedef unsigned char GLboolean;

// FSR 2 hooks (ravenfsr.cpp)
int ravenfsr_wanted(void); void ravenfsr_use_program(GLuint p); void ravenfsr_matrix(GLint loc, const float *m);
int ravenfsr_draw(void); GLuint ravenfsr_bind_texture(GLuint tex); void ravenfsr_frame_end(void);
static int fsr;                           // FSR 2 active: wrappers feed it
int ravenfg_wanted(void); void ravenfg_draw(GLuint prog, GLuint fbo);
static int fg;                            // frame generation hooks
static GLuint unit_tex[32];
GLuint ravengl_unit_tex(int u) { return u >= 0 && u < 32 ? unit_tex[u] : 0; }

static int tracing;                       // 1 while the trace frame is being logged
static long ndraw, ndraw_seg;
static GLuint cur_prog, cur_fbo;

static void *gpa(const char *n){
    static void *(*g)(const char *);
    if (!g) g = dlsym(RTLD_DEFAULT, "glXGetProcAddressARB");
    void *f = g ? g(n) : NULL;
    return f ? f : dlsym(RTLD_DEFAULT, n);
}

// GL 4.5 DSA queries (diagnostics only; fine on the dev box)
static void (*q_fbattach)(GLuint, GLenum, GLenum, GLint *);
static void (*q_texlevel)(GLuint, GLint, GLenum, GLint *);
static void (*q_rbparam)(GLuint, GLenum, GLint *);
static void (*q_progiv)(GLuint, GLenum, GLint *);
static void (*q_activeuni)(GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, char *);
static GLint (*q_uniloc)(GLuint, const char *);

static void describe_fbo(GLuint fbo){
    if (!fbo) { fprintf(stderr, "[gltrace]   fbo 0 = default\n"); return; }
    static const GLenum att[] = {0x8CE0,0x8CE1,0x8CE2,0x8CE3,0x8CE4,0x8CE5,0x8D00,0x8D20,0x821A};
    static const char *an[] = {"c0","c1","c2","c3","c4","c5","depth","stencil","ds"};
    for (int i = 0; i < 9; i++) {
        GLint type = 0, name = 0, w = 0, h = 0, fmt = 0;
        q_fbattach(fbo, att[i], 0x8CD0, &type);
        if (!type) continue;
        q_fbattach(fbo, att[i], 0x8CD1, &name);
        if (type == 0x1702) { q_texlevel(name, 0, 0x1000, &w); q_texlevel(name, 0, 0x1001, &h); q_texlevel(name, 0, 0x1003, &fmt); }
        else { q_rbparam(name, 0x8D42, &w); q_rbparam(name, 0x8D43, &h); q_rbparam(name, 0x8D44, &fmt); }
        fprintf(stderr, "[gltrace]   %s %s %d %dx%d fmt 0x%x\n", an[i], type == 0x1702 ? "tex" : "rb", name, w, h, fmt);
    }
}

static const char *uni_name(GLuint prog, GLint loc){
    static char buf[128]; GLint n = 0; buf[0] = 0;
    q_progiv(prog, 0x8B86, &n);   // GL_ACTIVE_UNIFORMS
    for (GLint i = 0; i < n; i++) {
        GLsizei l; GLint sz; GLenum ty; char nm[128];
        q_activeuni(prog, i, sizeof nm, &l, &sz, &ty, nm);
        if (q_uniloc(prog, nm) == loc) { snprintf(buf, sizeof buf, "%s", nm); break; }
    }
    return buf;
}
static void prog_uniforms(GLuint prog){
    static GLuint seen[4096]; static int ns;
    for (int i = 0; i < ns; i++) if (seen[i] == prog) return;
    if (ns < 4096) seen[ns++] = prog;
    GLint n = 0; q_progiv(prog, 0x8B86, &n);
    char line[2048]; int p = snprintf(line, sizeof line, "[gltrace]   prog %u uniforms:", prog);
    for (GLint i = 0; i < n && p < (int)sizeof line - 80; i++) {
        GLsizei l; GLint sz; GLenum ty; char nm[64];
        q_activeuni(prog, i, sizeof nm, &l, &sz, &ty, nm);
        p += snprintf(line + p, sizeof line - p, " %s", nm);
    }
    fprintf(stderr, "%s\n", line);
}

#define R(name, ret, args) static ret (*r_##name) args;
R(glBindFramebuffer, void, (GLenum, GLuint))
R(glUseProgram, void, (GLuint))
R(glUniformMatrix4fv, void, (GLint, GLsizei, GLboolean, const float *))
R(glUniform1i, void, (GLint, GLint))
R(glDrawElements, void, (GLenum, GLsizei, GLenum, const void *))
R(glDrawArrays, void, (GLenum, GLint, GLsizei))
R(glDrawElementsInstanced, void, (GLenum, GLsizei, GLenum, const void *, GLsizei))
R(glDrawRangeElements, void, (GLenum, GLuint, GLuint, GLsizei, GLenum, const void *))
R(glBlitFramebuffer, void, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum))
R(glClear, void, (GLbitfield))
R(glViewport, void, (GLint, GLint, GLsizei, GLsizei))
R(glDrawBuffers, void, (GLsizei, const GLenum *))
R(glBindTexture, void, (GLenum, GLuint))
R(glActiveTexture, void, (GLenum))
R(glClearColor, void, (float, float, float, float))
R(glClearBufferfv, void, (GLenum, GLint, const float *))

static void seg_end(void){ if (tracing && ndraw_seg) fprintf(stderr, "[gltrace]   ... %ld draws\n", ndraw_seg); ndraw_seg = 0; }
static void w_glBindFramebuffer(GLenum t, GLuint f){
    r_glBindFramebuffer(t, f);
    if (t != 0x8CA8) cur_fbo = f;
    if (!tracing) return;
    if (t == 0x8CA8) { fprintf(stderr, "[gltrace] bind READ fbo %u\n", f); return; }
    seg_end(); fprintf(stderr, "[gltrace] bind fbo %u (0x%x)\n", f, t); describe_fbo(f);
}
static void dump_src(GLuint p){
    static GLuint done_[64]; static int nd; const char *want = getenv("RAVENGFX_PROGSRC");
    if (!want || !p) return;
    char key[16]; snprintf(key, sizeof key, ",%u,", p); char w2[512]; snprintf(w2, sizeof w2, ",%s,", want);
    if (!strstr(w2, key)) return;
    for (int i = 0; i < nd; i++) if (done_[i] == p) return;
    if (nd < 64) done_[nd++] = p;
    void (*att)(GLuint, GLsizei, GLsizei *, GLuint *) = gpa("glGetAttachedShaders");
    void (*src)(GLuint, GLsizei, GLsizei *, char *) = gpa("glGetShaderSource");
    GLuint sh[8]; GLsizei n = 0; att(p, 8, &n, sh);
    char *buf = malloc(1 << 20);
    for (int i = 0; i < n; i++) { GLsizei l = 0; src(sh[i], 1 << 20, &l, buf); fprintf(stderr, "[progsrc %u.%d]\n%s\n[/progsrc]\n", p, i, buf); }
    free(buf);
}
static void w_glUseProgram(GLuint p){
    r_glUseProgram(p); cur_prog = p;
    if (fsr) ravenfsr_use_program(p);
    if (tracing) { fprintf(stderr, "[gltrace] use prog %u\n", p); if (p) { prog_uniforms(p); dump_src(p); } }
}
static void w_glUniformMatrix4fv(GLint loc, GLsizei n, GLboolean tr, const float *m){
    r_glUniformMatrix4fv(loc, n, tr, m);
    if (fsr && n == 1 && !tr) ravenfsr_matrix(loc, m);
    if (!tracing || !cur_prog) return;
    const char *nm = uni_name(cur_prog, loc);
    if (!strstr(nm, "roj") && !strstr(nm, "View")) return;
    fprintf(stderr, "[gltrace]   mat %s x%d%s:", nm, n, tr ? " T" : "");
    for (int i = 0; i < 16; i++) fprintf(stderr, " %.5g", m[i]);
    fprintf(stderr, "\n");
}
static void w_glUniform1i(GLint loc, GLint v){
    r_glUniform1i(loc, v);
    if (tracing && cur_prog) fprintf(stderr, "[gltrace]   int %s = %d\n", uni_name(cur_prog, loc), v);
}
static void w_glDrawElements(GLenum m, GLsizei c, GLenum t, const void *i){ if (fg) ravenfg_draw(cur_prog, cur_fbo); if (fsr && ravenfsr_draw()) return; r_glDrawElements(m,c,t,i); ndraw++; ndraw_seg++; }
static void w_glDrawArrays(GLenum m, GLint f, GLsizei c){ if (fg) ravenfg_draw(cur_prog, cur_fbo); if (fsr && ravenfsr_draw()) return; r_glDrawArrays(m,f,c); ndraw++; ndraw_seg++; }
static void w_glDrawElementsInstanced(GLenum m, GLsizei c, GLenum t, const void *i, GLsizei n){ if (fg) ravenfg_draw(cur_prog, cur_fbo); if (fsr && ravenfsr_draw()) return; r_glDrawElementsInstanced(m,c,t,i,n); ndraw++; ndraw_seg++; }
static void w_glDrawRangeElements(GLenum m, GLuint a, GLuint b, GLsizei c, GLenum t, const void *i){ if (fg) ravenfg_draw(cur_prog, cur_fbo); if (fsr && ravenfsr_draw()) return; r_glDrawRangeElements(m,a,b,c,t,i); ndraw++; ndraw_seg++; }
static void w_glBlitFramebuffer(GLint a,GLint b,GLint c,GLint d,GLint e,GLint f,GLint g,GLint h,GLbitfield m,GLenum fl){
    r_glBlitFramebuffer(a,b,c,d,e,f,g,h,m,fl);
    if (tracing) fprintf(stderr, "[gltrace] blit %d,%d-%d,%d -> %d,%d-%d,%d mask 0x%x\n", a,b,c,d,e,f,g,h,m);
}
static void w_glClearColor(float r, float g, float b, float a){ r_glClearColor(r,g,b,a); if (tracing) fprintf(stderr, "[gltrace]   clearcolor %g %g %g %g\n", r,g,b,a); }
static void w_glClearBufferfv(GLenum bf, GLint i, const float *v){ r_glClearBufferfv(bf,i,v); if (tracing) fprintf(stderr, "[gltrace]   clearbuffer 0x%x %d = %g %g %g %g\n", bf, i, v[0], bf == 0x1800 ? v[1] : 0, bf == 0x1800 ? v[2] : 0, bf == 0x1800 ? v[3] : 0); }
static void w_glClear(GLbitfield m){ r_glClear(m); if (tracing) fprintf(stderr, "[gltrace]   clear 0x%x\n", m); }
static void w_glViewport(GLint x, GLint y, GLsizei w, GLsizei h){ r_glViewport(x,y,w,h); if (tracing) fprintf(stderr, "[gltrace]   viewport %d,%d %dx%d\n", x,y,w,h); }
static void w_glDrawBuffers(GLsizei n, const GLenum *b){
    r_glDrawBuffers(n, b);
    if (tracing) { fprintf(stderr, "[gltrace]   drawbuffers"); for (int i = 0; i < n; i++) fprintf(stderr, " 0x%x", b[i]); fprintf(stderr, "\n"); }
}
static GLenum cur_unit;
static void w_glActiveTexture(GLenum u){ r_glActiveTexture(u); cur_unit = u; }
static void w_glBindTexture(GLenum t, GLuint x){
    if (t == 0x0DE1) { unsigned u = cur_unit ? cur_unit - 0x84C0 : 0; if (fsr) x = ravenfsr_bind_texture(x); if (u < 32) unit_tex[u] = x; }
    r_glBindTexture(t, x);
    if (tracing) { GLint w = 0, h = 0, f = 0; if (x) { q_texlevel(x, 0, 0x1000, &w); q_texlevel(x, 0, 0x1001, &h); q_texlevel(x, 0, 0x1003, &f); }
        fprintf(stderr, "[gltrace]   tex unit %u <- %u (%dx%d 0x%x)\n", cur_unit - 0x84C0, x, w, h, f); } }

struct ent { const char *n; void **real; void *wrap; };
#define E(name) { #name, (void **)&r_##name, (void *)w_##name }
static struct ent ents[] = {
    E(glBindFramebuffer), E(glUseProgram), E(glUniformMatrix4fv), E(glUniform1i), E(glDrawElements),
    E(glDrawArrays), E(glDrawElementsInstanced), E(glDrawRangeElements), E(glBlitFramebuffer),
    E(glClear), E(glViewport), E(glDrawBuffers), E(glBindTexture), E(glActiveTexture), E(glClearColor), E(glClearBufferfv),
};

static double trace_at = -1; static int installed, done;

void ravengl_trace_frame(double t)     // called from the present hook, before the real swap
{
    if (!installed) {
        installed = 1;
        const char *e = getenv("RAVENGFX_GLTRACE");
        fsr = ravenfsr_wanted(); fg = !getenv("RAVENGFX_NOGLHOOK") && ravenfg_wanted();
        if (!e && !fsr && !fg) return;
        trace_at = e ? atof(e) : -1;
        q_fbattach = gpa("glGetNamedFramebufferAttachmentParameteriv"); q_texlevel = gpa("glGetTextureLevelParameteriv");
        q_rbparam = gpa("glGetNamedRenderbufferParameteriv"); q_progiv = gpa("glGetProgramiv");
        q_activeuni = gpa("glGetActiveUniform"); q_uniloc = gpa("glGetUniformLocation");
        const unsigned NE = sizeof ents / sizeof *ents;
        uint64_t ra[32], rb[32];
        for (unsigned k = 0; k < NE; k++) { *ents[k].real = gpa(ents[k].n); ra[k] = (uint64_t)*ents[k].real; rb[k] = (uint64_t)dlsym(RTLD_DEFAULT, ents[k].n); }
        FILE *m = fopen("/proc/self/maps", "r"); char line[512];
        int total = 0;
        while (m && fgets(line, sizeof line, m)) {
            uint64_t a, b; char perm[5];
            if (sscanf(line, "%lx-%lx %4s", &a, &b, perm) != 3 || perm[0] != 'r' || perm[1] != 'w' || a >= 0x10000000UL) continue;
            for (uint64_t *q = (uint64_t *)a; q < (uint64_t *)b; q++) {
                if (*q < 0x10000000UL) continue;
                for (unsigned k = 0; k < NE; k++)
                    if (*q == ra[k] || (rb[k] && *q == rb[k])) {
                        fprintf(stderr, "[gltrace] %s slot %p%s\n", ents[k].n, (void *)q, *q == ra[k] ? "" : " (dlsym addr)");
                        *ents[k].real = (void *)*q; *q = (uint64_t)ents[k].wrap; total++; break;
                    }
            }
        }
        if (m) fclose(m);
        fprintf(stderr, "[gltrace] %d slots patched, trace at %.0fs\n", total, trace_at);
        return;
    }
    if (fsr) ravenfsr_frame_end();
    if (trace_at < 0 || done) return;
    if (tracing) { seg_end(); fprintf(stderr, "[gltrace] ==== end of frame, %ld draws\n", ndraw); tracing = 0; done = 1; return; }
    if (t >= trace_at) { tracing = 1; ndraw = 0; fprintf(stderr, "[gltrace] ==== frame start (fbo %u prog %u)\n", cur_fbo, cur_prog); }
}
