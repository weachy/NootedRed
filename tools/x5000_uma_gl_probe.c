#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <err.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

enum {
    texturesPerDraw = 5,
    retainedGroups = 3,
    MiB = 1U << 20,
    bufferCount = 9,
    bufferBytes = 64 * MiB,
};

static uint64_t counter(const char *name)
{
    uint64_t value = 0;
    size_t size = sizeof(value);
    if (sysctlbyname(name, &value, &size, NULL, 0) != 0) { err(1, "%s", name); }
    if (size != sizeof(value)) { errx(1, "Unexpected counter size for %s", name); }
    return value;
}

static void checkGL(const char *stage)
{
    const GLenum error = glGetError();
    if (error != GL_NO_ERROR) { errx(1, "%s failed with OpenGL error %#x", stage, error); }
}

static GLuint shader(GLenum type, const char *source)
{
    const GLuint object = glCreateShader(type);
    glShaderSource(object, 1, &source, NULL);
    glCompileShader(object);
    GLint ok = 0;
    glGetShaderiv(object, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        glGetShaderInfoLog(object, sizeof(log), NULL, log);
        errx(1, "Shader compilation failed: %s", log);
    }
    return object;
}

static GLuint readbackProgram(const char *vertexSource, const char *fragmentSource)
{
    const GLuint vertex = shader(GL_VERTEX_SHADER, vertexSource);
    const GLuint fragment = shader(GL_FRAGMENT_SHADER, fragmentSource);
    const GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glBindAttribLocation(program, 0, "value");
    glLinkProgram(program);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        glGetProgramInfoLog(program, sizeof(log), NULL, log);
        errx(1, "Program link failed: %s", log);
    }
    glDeleteShader(fragment);
    glDeleteShader(vertex);
    glUseProgram(program);
    return program;
}

static GLuint programForReadback(void)
{
    const char *vertexSource =
        "#version 150\n"
        "const vec2 vertices[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));\n"
        "void main() { gl_Position = vec4(vertices[gl_VertexID], 0.0, 1.0); }\n";
    const char *fragmentSource =
        "#version 150\n"
        "uniform sampler2D sources[5];\n"
        "out vec4 color;\n"
        "void main() {\n"
        "    int column = int(gl_FragCoord.x);\n"
        "    bool right = (int(gl_FragCoord.y) != 0);\n"
        "    if (column == 0) color = texelFetch(sources[0], right ? textureSize(sources[0], 0) - 1 : ivec2(0), 0);\n"
        "    if (column == 1) color = texelFetch(sources[1], right ? textureSize(sources[1], 0) - 1 : ivec2(0), 0);\n"
        "    if (column == 2) color = texelFetch(sources[2], right ? textureSize(sources[2], 0) - 1 : ivec2(0), 0);\n"
        "    if (column == 3) color = texelFetch(sources[3], right ? textureSize(sources[3], 0) - 1 : ivec2(0), 0);\n"
        "    if (column == 4) color = texelFetch(sources[4], right ? textureSize(sources[4], 0) - 1 : ivec2(0), 0);\n"
        "}\n";
    const GLuint program = readbackProgram(vertexSource, fragmentSource);
    const GLint locations[texturesPerDraw] = {0, 1, 2, 3, 4};
    const GLint uniform = glGetUniformLocation(program, "sources[0]");
    if (uniform == -1) { errx(1, "The readback program lost its source textures"); }
    glUniform1iv(uniform, texturesPerDraw, locations);
    return program;
}

static void allocateTexture(GLuint texture, GLsizei width, GLsizei height)
{
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    checkGL("texture allocation");
}

static void attach(GLuint texture)
{
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { errx(1, "Framebuffer incomplete"); }
}

static uint8_t sample(unsigned group, unsigned column, unsigned row)
{
    return (uint8_t)(13 + (group * texturesPerDraw + column) * 5 + row * 97);
}

static void fill(GLuint texture, GLsizei edge, unsigned group, unsigned column)
{
    allocateTexture(texture, edge, edge);
    attach(texture);
    glViewport(0, 0, edge, edge);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(sample(group, column, 0) / 255.0f, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    glScissor(edge / 2, 0, edge / 2, edge);
    glClearColor(sample(group, column, 1) / 255.0f, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    checkGL("texture rendering");
}

static void verifyDraw(const GLuint *textures, unsigned group)
{
    uint8_t out[2][texturesPerDraw][4] = {0};
    for (unsigned column = 0; column < texturesPerDraw; ++column) {
        glActiveTexture(GL_TEXTURE0 + column);
        glBindTexture(GL_TEXTURE_2D, textures[column]);
    }
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glReadPixels(0, 0, texturesPerDraw, 2, GL_RGBA, GL_UNSIGNED_BYTE, out);
    checkGL("multi-texture draw and readback");
    for (unsigned row = 0; row < 2; ++row) {
        for (unsigned column = 0; column < texturesPerDraw; ++column) {
            if (out[row][column][0] != sample(group, column, row) || out[row][column][1] != 0
                || out[row][column][2] != 0 || out[row][column][3] != 255) {
                errx(1, "Group %u texture %u edge %u failed readback: got %u %u %u %u",
                     group, column, row, out[row][column][0], out[row][column][1], out[row][column][2],
                     out[row][column][3]);
            }
        }
    }
}

static void verifyMultisampleFramebuffer(void)
{
    const GLsizei size = 1024;
    GLuint framebuffers[2] = {0};
    GLuint multisampleColor = 0;
    GLuint depthStencil = 0;
    GLuint resolved = 0;
    glGenFramebuffers(2, framebuffers);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffers[0]);
    glGenTextures(1, &multisampleColor);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, multisampleColor);
    glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, 4, GL_RGBA8, size, size, GL_TRUE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D_MULTISAMPLE, multisampleColor, 0);
    glGenRenderbuffers(1, &depthStencil);
    glBindRenderbuffer(GL_RENDERBUFFER, depthStencil);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, 4, GL_DEPTH24_STENCIL8, size, size);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depthStencil);
    checkGL("multisample attachment allocation");
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        errx(1, "Multisample framebuffer incomplete");
    }

    glViewport(0, 0, size, size);
    glClearColor(17.0f / 255.0f, 151.0f / 255.0f, 31.0f / 255.0f, 1.0f);
    glClearDepth(0.625);
    glClearStencil(23);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffers[1]);
    glGenTextures(1, &resolved);
    allocateTexture(resolved, size, size);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, resolved, 0);
    if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        errx(1, "Resolve framebuffer incomplete");
    }
    glBlitFramebuffer(0, 0, size, size, 0, 0, size, size, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffers[1]);
    uint8_t pixel[4] = {0};
    glReadPixels(size / 2, size / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    checkGL("multisample resolve and readback");
    if (pixel[0] != 17 || pixel[1] != 151 || pixel[2] != 31 || pixel[3] != 255) {
        errx(1, "Multisample resolve returned %u %u %u %u", pixel[0], pixel[1], pixel[2], pixel[3]);
    }
    glDeleteFramebuffers(2, framebuffers);
    glDeleteTextures(1, &multisampleColor);
    glDeleteTextures(1, &resolved);
    glDeleteRenderbuffers(1, &depthStencil);
    puts("OpenGL readback passed after 4x MSAA resolve with depth/stencil");
}

static void waitForPressureSignal(const char *message)
{
    puts(message);
    fflush(stdout);
    if (getchar() != '\n') { errx(1, "Pressure controller closed before readback"); }
}

static void verifyVertexBuffer(GLuint buffer, unsigned index, uint8_t firstChannel)
{
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    for (unsigned end = 0; end < 2; ++end) {
        const uintptr_t offset = end == 0 ? 0 : bufferBytes - 4;
        glVertexAttribPointer(0, 4, GL_UNSIGNED_BYTE, GL_TRUE, 0, (const void *)offset);
        glDrawArrays(GL_POINTS, 0, 1);
        uint8_t pixel[4] = {0};
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        checkGL("OpenGL vertex buffer readback");
        if (pixel[0] != firstChannel + end * 20 || pixel[1] != 23 || pixel[2] != 17 || pixel[3] != 255) {
            errx(1, "OpenGL vertex buffer %u failed readback at offset %lu", index, (unsigned long)offset);
        }
    }
}

static void verifyVertexBuffers(bool waitForPressure)
{
    const GLuint program = readbackProgram(
        "#version 150\n"
        "in vec4 value; out vec4 vertexColor;\n"
        "void main() { gl_Position = vec4(0, 0, 0, 1); vertexColor = value; }\n",
        "#version 150\n"
        "in vec4 vertexColor; out vec4 color;\n"
        "void main() { color = vertexColor; }\n");
    GLuint framebuffer = 0, output = 0, vertices = 0;
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glGenTextures(1, &output);
    allocateTexture(output, 1, 1);
    attach(output);
    glViewport(0, 0, 1, 1);
    glDisable(GL_DITHER);
    glGenVertexArrays(1, &vertices);
    glBindVertexArray(vertices);
    glEnableVertexAttribArray(0);

    const uint64_t before = counter("debug.nred_uma.data_buffers");
    GLuint buffers[bufferCount] = {0};
    glGenBuffers(bufferCount, buffers);
    for (unsigned index = 0; index < bufferCount; ++index) {
        const uint8_t first[4] = {index + 10, 23, 17, 255};
        const uint8_t last[4] = {index + 30, 23, 17, 255};
        glBindBuffer(GL_ARRAY_BUFFER, buffers[index]);
        glBufferData(GL_ARRAY_BUFFER, bufferBytes, NULL, GL_STATIC_DRAW);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(first), first);
        glBufferSubData(GL_ARRAY_BUFFER, bufferBytes - sizeof(last), sizeof(last), last);
        checkGL("OpenGL vertex buffer allocation and upload");
        verifyVertexBuffer(buffers[index], index, first[0]);
    }
    const uint64_t observed = counter("debug.nred_uma.data_buffers") - before;
    printf("OpenGL GPU readback passed for %u vertex buffers, %u MiB, with %llu additional pageable buffer allocations\n",
           bufferCount, bufferCount * bufferBytes / MiB, (unsigned long long)observed);
    if (observed < bufferCount) { errx(1, "Expected at least %u new pageable buffer allocations", bufferCount); }
    if (waitForPressure) {
        waitForPressureSignal("OpenGL vertex buffers initialized and verified, waiting for pressure");
        for (unsigned index = 0; index < bufferCount; ++index) { verifyVertexBuffer(buffers[index], index, index + 10); }
        puts("OpenGL readback passed with the original vertex buffers after pressure");
        for (unsigned index = 0; index < bufferCount; ++index) {
            const uint8_t first[4] = {index + 110, 23, 17, 255};
            const uint8_t last[4] = {index + 130, 23, 17, 255};
            glBindBuffer(GL_ARRAY_BUFFER, buffers[index]);
            glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(first), first);
            glBufferSubData(GL_ARRAY_BUFFER, bufferBytes - sizeof(last), sizeof(last), last);
            checkGL("OpenGL vertex buffer reupload");
            verifyVertexBuffer(buffers[index], index, first[0]);
        }
        puts("OpenGL readback passed for CPU updates to the original vertex buffers");
    }
    glDeleteVertexArrays(1, &vertices);
    glDeleteBuffers(bufferCount, buffers);
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(1, &output);
    glDeleteProgram(program);
}

int main(int argc, char **argv)
{
    const bool baseline = argc == 2 && strcmp(argv[1], "--baseline") == 0;
    const bool smoke = argc == 2 && strcmp(argv[1], "--smoke") == 0;
    const bool layouts = argc == 2 && strcmp(argv[1], "--layouts") == 0;
    const bool buffers = (argc == 2 || argc == 3) && strcmp(argv[1], "--buffers") == 0;
    const bool waitForPressure = (argc == 2 && strcmp(argv[1], "--wait-for-pressure") == 0)
        || (argc == 3 && buffers && strcmp(argv[2], "--wait-for-pressure") == 0);
    if (argc > 1 && !baseline && !smoke && !layouts && !waitForPressure && !(buffers && argc == 2)) {
        errx(2, "Usage: %s [--baseline | --smoke | --layouts | --wait-for-pressure | --buffers [--wait-for-pressure]]",
             argv[0]);
    }
    if (!baseline) {
        uint32_t active = 0;
        size_t length = sizeof(active);
        if (sysctlbyname("debug.nred_uma.active", &active, &length, NULL, 0) != 0 || active != 1) {
            errx(1, "debug.nred_uma.active is not 1");
        }
    }
    const uint64_t queries = baseline ? 0 : counter("debug.nred_uma.gl_budget_queries");
    const bool small = baseline || smoke || layouts;
    const unsigned groupCount = small ? 1 : retainedGroups;
    const GLsizei edge = small ? 256 : 4096;
    const uint64_t drawBytes = (uint64_t)edge * edge * 4 * (texturesPerDraw - 1) + (uint64_t)edge * edge;

    const CGLPixelFormatAttribute attributes[] = {
        kCGLPFAAccelerated, kCGLPFANoRecovery, kCGLPFAOpenGLProfile,
        (CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core, (CGLPixelFormatAttribute)0,
    };
    CGLPixelFormatObj format = NULL;
    CGLContextObj context = NULL;
    GLint matches = 0;
    CGLError error = CGLChoosePixelFormat(attributes, &format, &matches);
    if (error != kCGLNoError || format == NULL || matches == 0) {
        errx(1, "Could not select an accelerated OpenGL context: %s", CGLErrorString(error));
    }
    error = CGLCreateContext(format, NULL, &context);
    CGLReleasePixelFormat(format);
    if (error == kCGLNoError) { error = CGLSetCurrentContext(context); }
    if (error != kCGLNoError) { errx(1, "Could not activate OpenGL: %s", CGLErrorString(error)); }
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    if (renderer == NULL || strstr(renderer, "AMD") == NULL) {
        errx(1, "Expected the AMD hardware renderer, got %s", renderer != NULL ? renderer : "(null)");
    }
    if (buffers) {
        verifyVertexBuffers(waitForPressure);
        CGLSetCurrentContext(NULL);
        CGLReleaseContext(context);
        return 0;
    }
    printf("%s, %.1f MiB per draw, %.1f MiB across %u groups\n", renderer, (double)drawBytes / MiB,
           (double)drawBytes * groupCount / MiB, groupCount);
    fflush(stdout);

    const uint64_t beforeBytes = baseline ? 0 : counter("debug.nred_uma.bytes_live");
    const uint64_t beforeCommits = baseline ? 0 : counter("debug.nred_uma.task_commits");
    GLuint textures[retainedGroups][texturesPerDraw] = {{0}};
    GLuint framebuffer = 0, output = 0, vertices = 0;
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glGenVertexArrays(1, &vertices);
    glBindVertexArray(vertices);
    glDisable(GL_DITHER);
    for (unsigned group = 0; group < groupCount; ++group) {
        glGenTextures(texturesPerDraw, textures[group]);
        for (unsigned column = 0; column < texturesPerDraw; ++column) {
            fill(textures[group][column], column + 1 == texturesPerDraw ? edge / 2 : edge, group, column);
        }
    }
    glGenTextures(1, &output);
    allocateTexture(output, texturesPerDraw, 2);
    attach(output);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, texturesPerDraw, 2);
    const GLuint program = programForReadback();
    for (unsigned group = 0; group < groupCount; ++group) {
        verifyDraw(textures[group], group);
    }
    if (!baseline) {
        const uint64_t budgetKiB = counter("debug.nred_uma.gl_command_budget_kib");
        const uint64_t afterBytes = counter("debug.nred_uma.bytes_live");
        const uint64_t growth = afterBytes > beforeBytes ? afterBytes - beforeBytes : 0;
        printf("GL command budget %.1f MiB, managed primary growth %.1f MiB\n",
               (double)budgetKiB / 1024, (double)growth / MiB);
        if (counter("debug.nred_uma.gl_budget_sites") != 3 || counter("debug.nred_uma.gl_budget_queries") <= queries
            || budgetKiB <= drawBytes / 1024 || counter("debug.nred_uma.task_commits") <= beforeCommits
            || (!small && growth <= 512 * (uint64_t)MiB)) {
            errx(1, "The OpenGL workload did not demonstrate the unified budget and pageable primary backing");
        }
    }
    if (waitForPressure) {
        waitForPressureSignal("OpenGL textures initialized and verified, waiting for pressure");
        for (unsigned group = 0; group < groupCount; ++group) {
            verifyDraw(textures[group], group);
        }
        puts("OpenGL readback passed with the original textures after pressure");
    }
    glDeleteProgram(program);
    glDeleteVertexArrays(1, &vertices);
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(1, &output);
    for (unsigned group = 0; group < groupCount; ++group) { glDeleteTextures(texturesPerDraw, textures[group]); }
    if (layouts) { verifyMultisampleFramebuffer(); }
    CGLSetCurrentContext(NULL);
    CGLReleaseContext(context);
    puts("OpenGL GPU texture readback passed");
    return 0;
}
