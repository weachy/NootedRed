/*
 * gl_blend_probe.c - offscreen OpenGL blending / dual-source-blend probe for the
 * NootedRed issue #158 investigation (Chromium/Electron corruption + GPU resets
 * when the user-space AMDRadeonX5000GLDriver is used on an AMD Vega-family iGPU).
 *
 * This tool is a *harness*, not a fix. It renders into an FBO, reads pixels back
 * and compares them against a CPU reference. It never touches the on-disk driver,
 * and never takes over the display. An Objective-C build attaches a small,
 * unordered AppKit window; the C fallback uses a PBuffer.
 *
 * !! SAFETY !!
 * On the target machine (macOS 15.8.1, Renoir 0x1638, NootedRed loaded) a minimal
 * CGL/FBO program crashed on the first glClear() with
 *   AMDRadeonX5000GLDriver glrAMD_GFX9_UpdateHwExportBlockState
 *     <- glrAMD_GFX9_UpdateDrawBufferInfo <- glrATI_Hwl_DrawableChanged
 *     <- glrBindContextDrawFramebuffer <- gleUpdateDrawFramebufferState
 *     <- glClear_Exec
 * (SIGSEGV, ~/Library/Logs/DiagnosticReports/glblend2-2026-10-18-211253.ips)
 * and WindowServer GPU resets were logged around the same time. Therefore:
 *   - the DEFAULT mode is --inspect (context + glGetString only, no drawing);
 *   - --render refuses to run unless --yes is passed, and only draws after a
 *     PBuffer drawable has been attached (a missing drawable is what made the
 *     earlier probe crash in glClear);
 *   - do NOT run --render without the Lead's explicit approval and a recovery
 *     plan (see analysis/repro-plan.md and tools/README.md).
 *
 * Modes:
 *   --inspect (default)  create a CGL 4.1-core context, print GL_VENDOR /
 *                        GL_RENDERER / GL_VERSION, blend-relevant extensions,
 *                        GL_MAX_DUAL_SOURCE_DRAW_BUFFERS, glGetError.
 *   --exts               also print the full extension list.
 *   --link               inspect + compile/link the single- and dual-source
 *                        fragment shaders and bind frag data locations.
 *                        No draw calls are issued.
 *   --render             link + attach PBuffer drawable + FBO + clear/draw/
 *                        glReadPixels + CPU comparison. Requires --yes.
 *
 * Exit codes:
 *   0   success (all tests PASS, or only SKIPs where the driver lacks support)
 *   1   cannot create CGL pixel format / context
 *   2   shader compile/link failure (harness problem, not a render result)
 *   3   FBO incomplete
 *   10  at least one rendered pixel test MISMATCHed
 *   11  render mode unavailable (PBuffer drawable could not be attached);
 *       no clear/draw was attempted
 *   12  render refused without --yes
 *   124 render watchdog fired (SIGALRM)
 *   128+N terminated by signal N (SIGSEGV=139, SIGBUS=138, SIGABRT=134)
 *
 * Build: tools/build.sh  ->  build/tools/gl_blend_probe
 * Usage: build/tools/gl_blend_probe [--inspect] [--exts] [--link] [--render --yes]
 *        [--timeout SECONDS]
 *
 * Reference math for the dual-source tests (GL_EXT_blend_func_extended):
 *   C_result = C_src0 * S + C_dst * D
 *   S = C_src1.rgb for GL_SRC1_COLOR, S = C_src1.a for GL_SRC1_ALPHA,
 *   D = 1 - S for GL_ONE_MINUS_SRC1_*.
 * A driver that silently ignores the second (index=1) output instead blends with
 * src0 alpha, which produces a different, easily detectable pixel value.
 */

#define GL_SILENCE_DEPRECATION 1

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>

#include <errno.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <mach-o/dyld.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __OBJC__
#import <AppKit/AppKit.h>
#endif

#define PROBE_WIDTH 64
#define PROBE_HEIGHT 64
#define PIXEL_TOLERANCE 2

static bool wantExts = false;
static bool wantLink = false;
static bool wantRender = false;
static bool consentRender = false;
static unsigned timeoutSeconds = 60;
static unsigned selectedCase = 0;
static bool orderedDrawable = false;

static void rawNote(const char* text)
{
    // async-signal-safe: used from signal handlers as well
    size_t len = strlen(text);
    ssize_t ignored = write(STDERR_FILENO, text, len);
    (void)ignored;
}

static void onFatalSignal(int signo)
{
    if (signo == SIGSEGV || signo == SIGBUS || signo == SIGABRT) {
        // Best-effort diagnostic in this short-lived test process. Symbolizing
        // is not async-signal-safe; the independent SIGALRM watchdog remains.
        void* frames[64];
        int count = backtrace(frames, 64);
        backtrace_symbols_fd(frames, count, STDERR_FILENO);
    }
    switch (signo) {
        case SIGSEGV:
            rawNote("gl_blend_probe: SIGSEGV; inspect the stack before attributing it to the driver\n");
            _exit(128 + signo);
        case SIGBUS:
            rawNote("gl_blend_probe: SIGBUS; inspect the stack before attributing it to the driver\n");
            _exit(128 + signo);
        case SIGABRT:
            rawNote("gl_blend_probe: SIGABRT (possibly gpusKillClient / driver abort)\n");
            _exit(128 + signo);
        case SIGALRM:
            rawNote("gl_blend_probe: render watchdog fired (GPU may be hung); aborting\n");
            _exit(124);
        default:
            _exit(128 + signo);
    }
}

static void installSignalHandlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = onFatalSignal;
    sigemptyset(&sa.sa_mask);
    // SA_RESETHAND so a second fault terminates immediately instead of recursing.
    sa.sa_flags = SA_RESETHAND;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGALRM, &sa, NULL);
}

static void usage(const char* argv0)
{
    fprintf(stderr,
            "usage: %s [--inspect] [--exts] [--link] [--render --yes] [--timeout SECONDS] [--case 1..4] [--ordered-drawable]\n"
            "\n"
            "  --inspect  (default) context + GL strings/extensions only; no drawing\n"
            "  --exts     print the full GL extension list\n"
            "  --link     additionally compile/link shaders (no draw calls)\n"
            "  --render   FBO clear/draw/readback + CPU comparison; REQUIRES --yes\n"
            "  --yes      consent to the risky render path\n"
            "  --timeout  render watchdog in seconds (default 60)\n"
            "  --case     run one render case in its own process (1=solid, 2=alpha, 3=SRC1_COLOR, 4=SRC1_ALPHA)\n"
            "  --ordered-drawable  attach a double-buffered NSOpenGLView in a small visible window\n"
            "\n"
            "See tools/README.md and analysis/repro-plan.md before using --render.\n",
            argv0);
}

static bool parseArgs(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (!strcmp(a, "--inspect")) {
            wantRender = false;
        } else if (!strcmp(a, "--exts")) {
            wantExts = true;
        } else if (!strcmp(a, "--link")) {
            wantLink = true;
        } else if (!strcmp(a, "--render")) {
            wantRender = true;
        } else if (!strcmp(a, "--yes")) {
            consentRender = true;
        } else if (!strcmp(a, "--ordered-drawable")) {
            orderedDrawable = true;
        } else if (!strcmp(a, "--case")) {
            const char* value = i + 1 < argc ? argv[++i] : NULL;
            if (value == NULL || strlen(value) != 1 || value[0] < '1' || value[0] > '4') {
                fprintf(stderr, "--case needs a value from 1 to 4\n");
                return false;
            }
            selectedCase = (unsigned)(value[0] - '0');
        } else if (!strncmp(a, "--timeout", 9)) {
            const char* value = a[9] == '=' ? a + 10 : (i + 1 < argc ? argv[++i] : NULL);
            if (value == NULL) {
                fprintf(stderr, "--timeout needs a value\n");
                return false;
            }
            long parsed = strtol(value, NULL, 10);
            if (parsed >= 5 && parsed <= 600) {
                timeoutSeconds = (unsigned)parsed;
            } else {
                fprintf(stderr, "invalid --timeout %s (5..600)\n", value);
                return false;
            }
        } else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage(argv[0]);
            return false;
        } else {
            fprintf(stderr, "unknown argument: %s\n", a);
            usage(argv[0]);
            return false;
        }
    }
    if (wantLink || wantRender) {
        // --link/--render build on the inspect information; nothing extra to do.
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* GL helpers                                                         */
/* ------------------------------------------------------------------ */

static const char* glErrorName(GLenum error)
{
    switch (error) {
        case GL_NO_ERROR:
            return "GL_NO_ERROR";
        case GL_INVALID_ENUM:
            return "GL_INVALID_ENUM";
        case GL_INVALID_VALUE:
            return "GL_INVALID_VALUE";
        case GL_INVALID_OPERATION:
            return "GL_INVALID_OPERATION";
        case GL_INVALID_FRAMEBUFFER_OPERATION:
            return "GL_INVALID_FRAMEBUFFER_OPERATION";
        case GL_OUT_OF_MEMORY:
            return "GL_OUT_OF_MEMORY";
        default:
            return "GL_ERROR_UNKNOWN";
    }
}

static bool reportGlError(const char* where)
{
    GLenum error = glGetError();
    if (error == GL_NO_ERROR) {
        return true;
    }
    printf("  [warn] glGetError after %s: 0x%04x (%s)\n", where, error, glErrorName(error));
    return false;
}

static bool extensionListed(const char* needle)
{
    GLint count = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &count);
    for (GLint i = 0; i < count; ++i) {
        const char* e = (const char*)glGetStringi(GL_EXTENSIONS, (GLuint)i);
        if (e != NULL && !strcmp(e, needle)) {
            return true;
        }
    }
    return false;
}

static GLuint compileShader(GLenum type, const char* source, const char* label)
{
    GLuint shader = glCreateShader(type);
    if (shader == 0) {
        fprintf(stderr, "glCreateShader failed for %s\n", label);
        return 0;
    }
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096] = {0};
        glGetShaderInfoLog(shader, sizeof(log) - 1, NULL, log);
        fprintf(stderr, "shader compile failed (%s):\n%s\n", label, log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint linkProgram(const char* vsSource, const char* fsSource, const char* label,
                          bool dualSource, bool* boundIndexed)
{
    GLuint vs = compileShader(GL_VERTEX_SHADER, vsSource, label);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fsSource, label);
    if (vs == 0 || fs == 0) {
        if (vs != 0) {
            glDeleteShader(vs);
        }
        if (fs != 0) {
            glDeleteShader(fs);
        }
        return 0;
    }
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    if (dualSource) {
        glBindFragDataLocationIndexed(program, 0, 0, "c0");
        glBindFragDataLocationIndexed(program, 0, 1, "c1");
    } else {
        glBindFragDataLocation(program, 0, "o");
    }
    glLinkProgram(program);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096] = {0};
        glGetProgramInfoLog(program, sizeof(log) - 1, NULL, log);
        fprintf(stderr, "program link failed (%s):\n%s\n", label, log);
        glDeleteProgram(program);
        program = 0;
    } else if (dualSource && boundIndexed != NULL) {
        *boundIndexed = glGetFragDataIndex(program, "c1") == 1;
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return program;
}

/* ------------------------------------------------------------------ */
/* Shaders                                                            */
/* ------------------------------------------------------------------ */

static const char* kVertexShader =
    "#version 410 core\n"
    "void main() {\n"
    "    vec2 p = vec2((gl_VertexID == 1) ? 3.0 : -1.0,\n"
    "                  (gl_VertexID == 2) ? 3.0 : -1.0);\n"
    "    gl_Position = vec4(p, 0.0, 1.0);\n"
    "}\n";

static const char* kSolidRedFragment =
    "#version 410 core\n"
    "out vec4 o;\n"
    "void main() { o = vec4(1.0, 0.0, 0.0, 1.0); }\n";

static const char* kAlphaHalfFragment =
    "#version 410 core\n"
    "out vec4 o;\n"
    "void main() { o = vec4(1.0, 0.0, 0.0, 0.5); }\n";

/* c0 and c1 are both declared as (location=0,index=*) outputs. */
static const char* kDualSourceFragment =
    "#version 410 core\n"
    "layout(location = 0, index = 0) out vec4 c0;\n"
    "layout(location = 0, index = 1) out vec4 c1;\n"
    "void main() {\n"
    "    c0 = vec4(1.0, 0.0, 0.0, 1.0);\n"
    "    c1 = vec4(0.25, 0.5, 0.75, 1.0);\n"
    "}\n";

/* The same geometry, but c1.a is the interesting value (0.5) while c0.a is 1.0,
 * so SRC1_ALPHA gives a result that data-format mistakes cannot accidentally
 * produce. */
static const char* kDualSourceAlphaFragment =
    "#version 410 core\n"
    "layout(location = 0, index = 0) out vec4 c0;\n"
    "layout(location = 0, index = 1) out vec4 c1;\n"
    "void main() {\n"
    "    c0 = vec4(0.5, 0.25, 0.75, 1.0);\n"
    "    c1 = vec4(0.25, 0.5, 0.75, 0.5);\n"
    "}\n";

/* ------------------------------------------------------------------ */
/* CPU reference                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned char rgba[4];
} ExpectedPixel;

static unsigned char quantize(double value)
{
    double clamped = value < 0.0 ? 0.0 : (value > 1.0 ? 1.0 : value);
    long scaled = lround(clamped * 255.0);
    return (unsigned char)(scaled < 0 ? 0 : (scaled > 255 ? 255 : scaled));
}

static ExpectedPixel expectedOver(const double src[4], const double dst[4], const double factor[4])
{
    ExpectedPixel out;
    for (int i = 0; i < 4; ++i) {
        out.rgba[i] = quantize(src[i] * factor[i] + dst[i] * (1.0 - factor[i]));
    }
    return out;
}

/*
 * Compare a read-back pixel against the expected one. For dual-source tests the
 * alpha channel is informational only: GL_SRC1_* only specifies the RGB factor
 * and implementations differ in which alpha output feeds the alpha blend.
 */
static bool pixelMatches(const unsigned char got[4], ExpectedPixel expected, bool checkAlpha,
                         int* worst)
{
    bool ok = true;
    int delta = 0;
    for (int i = 0; i < 4; ++i) {
        if (i == 3 && !checkAlpha) {
            continue;
        }
        int d = (int)got[i] - (int)expected.rgba[i];
        if (d < 0) {
            d = -d;
        }
        if (d > delta) {
            delta = d;
        }
        if (d > PIXEL_TOLERANCE) {
            ok = false;
        }
    }
    if (worst != NULL) {
        *worst = delta;
    }
    return ok;
}

/* ------------------------------------------------------------------ */
/* Render path                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    const char* name;
    const char* program; /* filled at runtime */
    GLenum srcRgb;
    GLenum dstRgb;
    bool dualSource;
    bool checkAlpha;
    ExpectedPixel expected;
} BlendCase;

static bool runRenderCases(GLuint solidProgram, GLuint alphaProgram, GLuint dualProgram,
                           GLuint dualAlphaProgram, GLuint fbo, GLuint texture, bool dualSupported)
{
    const double blue[4] = {0.0, 0.0, 1.0, 1.0};
    const double red[4] = {1.0, 0.0, 0.0, 1.0};
    const double src1Color[4] = {0.25, 0.5, 0.75, 0.5};
    const double opaque[4] = {1.0, 1.0, 1.0, 1.0};
    const double halfFactor[4] = {0.5, 0.5, 0.5, 0.5};

    ExpectedPixel expSolid = expectedOver(red, blue, opaque);
    ExpectedPixel expAlpha = expectedOver(red, blue, halfFactor);
    // glBlendFuncSeparate below uses ONE/ZERO for alpha: source alpha replaces
    // destination alpha, independently of the RGB blend factors.
    expAlpha.rgba[3] = quantize(0.5);

    GLuint vao = 0;
    printf("[stage] creating VAO and binding framebuffer\n");
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, PROBE_WIDTH, PROBE_HEIGHT);
    glDisable(GL_DITHER);
    glDisable(GL_FRAMEBUFFER_SRGB);

    int failures = 0;
    int ran = 0;

    struct {
        const char* name;
        GLuint program;
        bool blend;
        GLenum srcRgb;
        GLenum dstRgb;
        ExpectedPixel expected;
        bool checkAlpha;
        bool dual;
    } cases[] = {
        {"solid no-blend red over blue", solidProgram, false, GL_ONE, GL_ZERO, expSolid, true, false},
        {"alpha SRC_ALPHA/ONE_MINUS_SRC_ALPHA", alphaProgram, true, GL_SRC_ALPHA,
         GL_ONE_MINUS_SRC_ALPHA, expAlpha, true, false},
        {"dual-source SRC1_COLOR", dualProgram, true, GL_SRC1_COLOR, GL_ONE_MINUS_SRC1_COLOR,
         expectedOver(red, blue, src1Color), false, true},
        {"dual-source SRC1_ALPHA", dualAlphaProgram, true, GL_SRC1_ALPHA,
         GL_ONE_MINUS_SRC1_ALPHA, (ExpectedPixel){{0, 0, 0, 255}}, false, true},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        if (selectedCase != 0 && selectedCase != i + 1) {
            continue;
        }
        if (cases[i].dual && !dualSupported) {
            printf("  SKIP  %-40s (driver does not advertise dual-source blend)\n", cases[i].name);
            continue;
        }
        if (cases[i].program == 0) {
            printf("  SKIP  %-40s (program unavailable)\n", cases[i].name);
            continue;
        }

        // expected pixel for the SRC1_ALPHA dual-source case
        if (cases[i].dual && cases[i].srcRgb == GL_SRC1_ALPHA) {
            const double c0[4] = {0.5, 0.25, 0.75, 1.0};
            const double factor = 0.5; // c1.a
            cases[i].expected.rgba[0] = quantize(c0[0] * factor + blue[0] * (1.0 - factor));
            cases[i].expected.rgba[1] = quantize(c0[1] * factor + blue[1] * (1.0 - factor));
            cases[i].expected.rgba[2] = quantize(c0[2] * factor + blue[2] * (1.0 - factor));
            cases[i].expected.rgba[3] = 255;
        }

        printf("[stage] case %zu clear\n", i + 1);
        glClearColor((GLfloat)blue[0], (GLfloat)blue[1], (GLfloat)blue[2], (GLfloat)blue[3]);
        glClear(GL_COLOR_BUFFER_BIT);

        if (cases[i].blend) {
            glEnable(GL_BLEND);
            glBlendFuncSeparate(cases[i].srcRgb, cases[i].dstRgb, GL_ONE, GL_ZERO);
        } else {
            glDisable(GL_BLEND);
        }
        glUseProgram(cases[i].program);
        printf("[stage] case %zu draw\n", i + 1);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        printf("[stage] case %zu finish\n", i + 1);
        glFinish();

        printf("[stage] case %zu readback\n", i + 1);
        unsigned char got[4] = {0, 0, 0, 0};
        glReadPixels(PROBE_WIDTH / 2, PROBE_HEIGHT / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, got);

        int worst = 0;
        bool ok = pixelMatches(got, cases[i].expected, cases[i].checkAlpha, &worst);
        printf("  %s  %-40s got (%3u,%3u,%3u,%3u) expect (%3u,%3u,%3u,%3u) maxdelta=%d\n",
               ok ? "PASS" : "FAIL", cases[i].name, got[0], got[1], got[2], got[3],
               cases[i].expected.rgba[0], cases[i].expected.rgba[1], cases[i].expected.rgba[2],
               cases[i].expected.rgba[3], worst);
        ++ran;
        if (!ok) {
            ++failures;
        }

        if (!reportGlError(cases[i].name)) {
            // Keep going: a GL error is informative but not necessarily fatal.
        }
    }

    glDisable(GL_BLEND);
    glBindVertexArray(0);
    glDeleteVertexArrays(1, &vao);
    (void)texture;

    printf("\n  %d case(s) executed, %d mismatch(es)\n", ran, failures);
    return failures == 0;
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char** argv)
{
    if (!parseArgs(argc, argv)) {
        return 1;
    }
    installSignalHandlers();
    setvbuf(stdout, NULL, _IONBF, 0);
    // Include context/drawable setup in the watchdog, not only glDrawArrays.
    alarm(timeoutSeconds);

    if (wantRender && !consentRender) {
        fprintf(stderr,
                "refusing --render without --yes.\n"
                "The render path has already SIGSEGV'd in AMDRadeonX5000GLDriver and coincided\n"
                "with WindowServer GPU resets on this machine (2026-10-18). Get Lead approval\n"
                "and read analysis/repro-plan.md before consenting.\n");
        return 12;
    }

    printf("gl_blend_probe: mode=%s\n", wantRender ? "render" : (wantLink ? "link" : "inspect"));

    // Pixel format attributes. The PBuffer attribute is only requested for the
    // render path: it is what lets us attach a *valid* offscreen drawable, which
    // the earlier crash probe was missing.
    CGLPixelFormatAttribute attrs[16];
    int n = 0;
    attrs[n++] = kCGLPFAAccelerated;
    attrs[n++] = kCGLPFAOpenGLProfile;
    attrs[n++] = (CGLPixelFormatAttribute)kCGLOGLPVersion_GL4_Core;
    attrs[n++] = kCGLPFAColorSize;
    attrs[n++] = (CGLPixelFormatAttribute)32;
    attrs[n++] = kCGLPFAAlphaSize;
    attrs[n++] = (CGLPixelFormatAttribute)8;
    if (wantRender) {
#ifdef __OBJC__
        if (orderedDrawable) { attrs[n++] = kCGLPFADoubleBuffer; }
#endif
#ifndef __OBJC__
        attrs[n++] = kCGLPFAPBuffer;
#endif
    }
    attrs[n] = (CGLPixelFormatAttribute)0;

    CGLPixelFormatObj pixelFormat = NULL;
    GLint numPixelFormats = 0;
    CGLError error = CGLChoosePixelFormat(attrs, &pixelFormat, &numPixelFormats);
    if (error != kCGLNoError || pixelFormat == NULL) {
        fprintf(stderr, "CGLChoosePixelFormat failed: %s (%d) - is an accelerated GL renderer present?\n",
                CGLErrorString(error), (int)error);
        return 1;
    }

    CGLContextObj context = NULL;
    error = CGLCreateContext(pixelFormat, NULL, &context);
    if (error != kCGLNoError || context == NULL) {
        fprintf(stderr, "CGLCreateContext failed: %s (%d)\n", CGLErrorString(error), (int)error);
        CGLDestroyPixelFormat(pixelFormat);
        return 1;
    }
    CGLSetCurrentContext(context);

    const GLubyte* vendor = glGetString(GL_VENDOR);
    const GLubyte* renderer = glGetString(GL_RENDERER);
    const GLubyte* version = glGetString(GL_VERSION);
    const GLubyte* glsl = glGetString(GL_SHADING_LANGUAGE_VERSION);

    printf("GL_VENDOR   = %s\n", vendor ? (const char*)vendor : "(null)");
    printf("GL_RENDERER = %s\n", renderer ? (const char*)renderer : "(null)");
    printf("GL_VERSION  = %s\n", version ? (const char*)version : "(null)");
    printf("GLSL        = %s\n", glsl ? (const char*)glsl : "(null)");

    bool arbDual = extensionListed("GL_ARB_blend_func_extended");
    bool extDual = extensionListed("GL_EXT_blend_func_extended");
    printf("GL_ARB_blend_func_extended = %s\n", arbDual ? "present" : "MISSING");
    printf("GL_EXT_blend_func_extended = %s\n", extDual ? "present" : "MISSING");

    GLint maxDrawBuffers = 0;
    GLint maxDualSource = 0;
    GLint maxColorAttachments = 0;
    glGetIntegerv(GL_MAX_DRAW_BUFFERS, &maxDrawBuffers);
    glGetIntegerv(GL_MAX_DUAL_SOURCE_DRAW_BUFFERS, &maxDualSource);
    glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS, &maxColorAttachments);
    printf("GL_MAX_DRAW_BUFFERS = %d, GL_MAX_DUAL_SOURCE_DRAW_BUFFERS = %d, "
           "GL_MAX_COLOR_ATTACHMENTS = %d\n",
           maxDrawBuffers, maxDualSource, maxColorAttachments);

    GLint extensionCount = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &extensionCount);
    printf("extension count = %d\n", extensionCount);
    if (wantExts) {
        for (GLint i = 0; i < extensionCount; ++i) {
            const char* e = (const char*)glGetStringi(GL_EXTENSIONS, (GLuint)i);
            if (e != NULL) {
                printf("  ext[%d] = %s\n", i, e);
            }
        }
    }
    reportGlError("GL string / extension queries");
    for (uint32_t imageIndex = 0; imageIndex < _dyld_image_count(); ++imageIndex) {
        const char* imageName = _dyld_get_image_name(imageIndex);
        if (imageName != NULL && strstr(imageName, "AMDRadeonX5000GLDriver") != NULL) {
            printf("GL_DRIVER_IMAGE = %s, base=%p\n", imageName,
                   (const void*)_dyld_get_image_header(imageIndex));
        }
    }

    bool dualSupported = (arbDual || extDual) && maxDualSource > 0;

    bool linkedIndexed = false;
    GLuint solidProgram = 0;
    GLuint alphaProgram = 0;
    GLuint dualProgram = 0;
    GLuint dualAlphaProgram = 0;

    if (wantLink || wantRender) {
        printf("\n[link] compiling programs (no draw calls issued)\n");
        solidProgram = linkProgram(kVertexShader, kSolidRedFragment, "solid", false, NULL);
        alphaProgram = linkProgram(kVertexShader, kAlphaHalfFragment, "alpha", false, NULL);
        if (dualSupported) {
            dualProgram = linkProgram(kVertexShader, kDualSourceFragment, "dual-source", true,
                                      &linkedIndexed);
            dualAlphaProgram = linkProgram(kVertexShader, kDualSourceAlphaFragment,
                                           "dual-source-alpha", true, NULL);
            printf("  c1 bound to frag data index 1: %s\n", linkedIndexed ? "yes" : "NO");
            if (linkedIndexed) {
                printf("  dual-source shaded linked: %s, %s\n",
                       dualProgram != 0 ? "SRC1_COLOR ok" : "SRC1_COLOR FAILED",
                       dualAlphaProgram != 0 ? "SRC1_ALPHA ok" : "SRC1_ALPHA FAILED");
            }
        } else {
            printf("  dual-source programs skipped: extension/limit not advertised\n");
        }
        if (solidProgram == 0 || alphaProgram == 0) {
            fprintf(stderr, "basic shader programs could not be linked; aborting\n");
            CGLSetCurrentContext(NULL);
            CGLDestroyContext(context);
            CGLDestroyPixelFormat(pixelFormat);
            return 2;
        }
        if (dualSupported && (dualProgram == 0 || dualAlphaProgram == 0)) {
            fprintf(stderr, "dual-source shader programs could not be linked; that is a repro signal\n");
            // Not a harness failure: the driver refused the feature. Continue to
            // render mode only for the non-dual tests.
            dualSupported = false;
        }
    }

    if (!wantRender) {
        if (wantLink) {
            printf("\n[link] done: no clear/draw/readback; context setup may submit initialization work.\n");
        } else {
            printf("\n[inspect] done: no drawing/drawable touched.\n");
        }
        if (solidProgram != 0) {
            glDeleteProgram(solidProgram);
        }
        if (alphaProgram != 0) {
            glDeleteProgram(alphaProgram);
        }
        if (dualProgram != 0) {
            glDeleteProgram(dualProgram);
        }
        if (dualAlphaProgram != 0) {
            glDeleteProgram(dualAlphaProgram);
        }
        CGLSetCurrentContext(NULL);
        CGLDestroyContext(context);
        CGLDestroyPixelFormat(pixelFormat);
        return 0;
    }

    // ---------------- render path ----------------
#ifdef __OBJC__
    // An optional, process-local diagnostic library can stop submission before
    // any drawable/drawing work. Normal builds have no dependency on it.
    void (*captureArm)(void) = dlsym(RTLD_DEFAULT, "NRedCaptureArm");
    if (captureArm != NULL) { captureArm(); }
    printf("\n[render] attaching %s AppKit drawable before clear/draw\n",
           orderedDrawable ? "an ordered" : "an unordered");
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
    __attribute__((objc_precise_lifetime)) NSWindow* window =
        [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, PROBE_WIDTH, PROBE_HEIGHT)
                                   styleMask:NSWindowStyleMaskBorderless
                                     backing:NSBackingStoreBuffered
                                       defer:NO];
    __attribute__((objc_precise_lifetime)) NSOpenGLPixelFormat* appPixelFormat =
        [[NSOpenGLPixelFormat alloc] initWithCGLPixelFormatObj:pixelFormat];
    __attribute__((objc_precise_lifetime)) NSOpenGLView* view =
        [[NSOpenGLView alloc] initWithFrame:NSMakeRect(0, 0, PROBE_WIDTH, PROBE_HEIGHT)
                               pixelFormat:appPixelFormat];
    [window setContentView:view];
    __attribute__((objc_precise_lifetime)) NSOpenGLContext* drawableContext =
        [[NSOpenGLContext alloc] initWithCGLContextObj:context];
    if (window == nil || view == nil || drawableContext == nil) {
        fprintf(stderr, "AppKit drawable allocation failed; no draw attempted\n");
        return 11;
    }
    [view setOpenGLContext:drawableContext];
    [drawableContext setView:view];
    if (orderedDrawable) {
        [window orderBack:nil];
        [window displayIfNeeded];
        printf("[render] ordered NSOpenGLView window=%ld visible=%d double-buffered=1\n",
               (long)[window windowNumber], (int)[window isVisible]);
    }
    [drawableContext makeCurrentContext];
    [drawableContext update];
    [view prepareOpenGL];
#else
    printf("\n[render] attaching PBuffer drawable before any clear/draw\n");
    CGLPBufferObj pbuffer = NULL;
    error = CGLCreatePBuffer(PROBE_WIDTH, PROBE_HEIGHT, GL_TEXTURE_2D, GL_RGBA, 0, &pbuffer);
    if (error != kCGLNoError || pbuffer == NULL) {
        fprintf(stderr, "CGLCreatePBuffer failed: %s (%d)\n", CGLErrorString(error), (int)error);
        CGLSetCurrentContext(NULL);
        CGLDestroyContext(context);
        CGLDestroyPixelFormat(pixelFormat);
        return 11;
    }
    error = CGLSetPBuffer(context, pbuffer, 0, 0, 0);
    if (error != kCGLNoError) {
        fprintf(stderr, "CGLSetPBuffer failed: %s (%d) - refusing to clear/draw without a drawable\n",
                CGLErrorString(error), (int)error);
        CGLDestroyPBuffer(pbuffer);
        CGLSetCurrentContext(NULL);
        CGLDestroyContext(context);
        CGLDestroyPixelFormat(pixelFormat);
        return 11;
    }
#endif
    CGLFlushDrawable(context);
    reportGlError("drawable attach");

    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, PROBE_WIDTH, PROBE_HEIGHT, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "FBO incomplete; refusing to draw\n");
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &texture);
#ifdef __OBJC__
        [drawableContext clearDrawable];
#else
        CGLDestroyPBuffer(pbuffer);
#endif
        CGLSetCurrentContext(NULL);
        CGLDestroyContext(context);
        CGLDestroyPixelFormat(pixelFormat);
        return 3;
    }

    printf("[render] watchdog: %u s\n", timeoutSeconds);
    alarm(timeoutSeconds);
    bool passed = runRenderCases(solidProgram, alphaProgram, dualProgram, dualAlphaProgram, fbo,
                                 texture, dualSupported);
    alarm(0);

    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &texture);
    if (solidProgram != 0) {
        glDeleteProgram(solidProgram);
    }
    if (alphaProgram != 0) {
        glDeleteProgram(alphaProgram);
    }
    if (dualProgram != 0) {
        glDeleteProgram(dualProgram);
    }
    if (dualAlphaProgram != 0) {
        glDeleteProgram(dualAlphaProgram);
    }
#ifdef __OBJC__
    [drawableContext clearDrawable];
#else
    CGLDestroyPBuffer(pbuffer);
#endif
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(context);
    CGLDestroyPixelFormat(pixelFormat);

    if (!passed) {
        printf("\nRESULT: MISMATCH - output differs from the CPU reference; the GL blend path is not trustworthy\n");
        return 10;
    }
    printf("\nRESULT: PASS - rendered output matches the CPU reference\n");
    return 0;
}
