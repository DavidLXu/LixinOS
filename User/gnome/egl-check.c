#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <string.h>
#include "lixin-gui.h"

enum { WIDTH = 320, HEIGHT = 200 };
static unsigned char pixels[WIDTH * HEIGHT * 4];

static GLuint shader(GLenum type, const char *source)
{
    GLuint id = glCreateShader(type);
    if (!id) return 0;
    glShaderSource(id, 1, &source, NULL);
    glCompileShader(id);
    GLint compiled = GL_FALSE;
    glGetShaderiv(id, GL_COMPILE_STATUS, &compiled);
    if (compiled) return id;
    char message[512];
    glGetShaderInfoLog(id, sizeof(message), NULL, message);
    fprintf(stderr, "Shader compilation failed: %s\n", message);
    glDeleteShader(id);
    return 0;
}

static int matches_rgba(const unsigned char *pixel, int green, int blue)
{
    return pixel[0] >= 31 && pixel[0] <= 33 &&
           pixel[1] >= green-1 && pixel[1] <= green+1 &&
           pixel[2] >= blue-1 && pixel[2] <= blue+1 && pixel[3] == 255;
}

static int draw(void)
{
    GLuint vertex = shader(GL_VERTEX_SHADER, "attribute vec2 position;"
                            "void main() { gl_Position = vec4(position, 0.0, 1.0); }");
    GLuint fragment = shader(GL_FRAGMENT_SHADER, "precision mediump float;"
                              "void main() { gl_FragColor = vec4(0.125, 0.75, 0.5, 1.0); }");
    GLuint program = glCreateProgram();
    int result = -1;
    if (!vertex || !fragment || !program) goto done;
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glBindAttribLocation(program, 0, "position");
    glLinkProgram(program);
    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char message[512];
        glGetProgramInfoLog(program, sizeof(message), NULL, message);
        fprintf(stderr, "Shader link failed: %s\n", message);
        goto done;
    }

    const GLfloat points[] = {-.8f, -.7f, .8f, -.7f, 0.f, .8f};
    glViewport(0, 0, WIDTH, HEIGHT);
    glClearColor(.125f, .25f, .375f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(program);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, points);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisableVertexAttribArray(0);
    glReadPixels(0, 0, WIDTH, HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    const unsigned char *center = pixels + ((HEIGHT / 2) * WIDTH + WIDTH / 2) * 4;
    GLenum error = glGetError();
    printf("GLES pixels: background=%u,%u,%u,%u center=%u,%u,%u,%u\n",
           pixels[0], pixels[1], pixels[2], pixels[3], center[0], center[1], center[2], center[3]);
    if (error || !matches_rgba(pixels, 64, 96) || !matches_rgba(center, 191, 128)) {
        fprintf(stderr, "Unexpected GLES result (error=%#x, background=%u, center=%u)\n",
                (unsigned)error, pixels[1], center[1]);
        goto done;
    }
    result = 0;

done:
    if (program) glDeleteProgram(program);
    if (vertex) glDeleteShader(vertex);
    if (fragment) glDeleteShader(fragment);
    return result;
}

static int present(void)
{
    struct lx_canvas canvas;
    if (lx_gui_open(&canvas) < 0) return -1;
    unsigned w = canvas.width < WIDTH ? canvas.width : WIDTH;
    unsigned h = canvas.height < HEIGHT ? canvas.height : HEIGHT;
    unsigned left = (canvas.width - w) / 2;
    unsigned top = (canvas.height - h) / 2;
    lx_gui_rect(&canvas, 0, 0, canvas.width, canvas.height, 0x121a28);
    for (unsigned y = 0; y < h; ++y) {
        for (unsigned x = 0; x < w; ++x) {
            const unsigned char *p = pixels + ((HEIGHT - 1 - y) * WIDTH + x) * 4;
            canvas.pixels[(top + y) * canvas.width + left + x] =
                (unsigned)p[0] << 16 | (unsigned)p[1] << 8 | p[2];
        }
    }
    if (lx_gui_present(&canvas) < 0) return -1;
    for (;;) {
        int key = lx_timed_byte(50);
        if (key == 27 || key == 'q' || key == 'Q') return 0;
    }
}

int main(int argc, char **argv)
{
    int headless = argc == 2 && strcmp(argv[1], "--headless") == 0;
    if (argc != 1 && !headless) {
        fprintf(stderr, "usage: lixin-egl-check [--headless]\n");
        return 2;
    }
    const EGLint config_attrs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE
    };
    const EGLint surface_attrs[] = {EGL_WIDTH, WIDTH, EGL_HEIGHT, HEIGHT, EGL_NONE};
    const EGLint context_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLDisplay display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    int status = 1;
    EGLint count;
    EGLConfig config;
    const char *stage = "initialize surfaceless display";
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, NULL, NULL)) goto fail;
    stage = "choose GLES config";
    if (!eglBindAPI(EGL_OPENGL_ES_API) ||
        !eglChooseConfig(display, config_attrs, &config, 1, &count) || !count)
        goto fail;
    stage = "create pbuffer";
    surface = eglCreatePbufferSurface(display, config, surface_attrs);
    if (surface == EGL_NO_SURFACE) goto fail;
    stage = "create GLES context";
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attrs);
    if (context == EGL_NO_CONTEXT) goto fail;
    stage = "make context current";
    if (!eglMakeCurrent(display, surface, surface, context)) goto fail;
    if (draw() == 0 && (headless || present() == 0)) {
        printf("Mesa EGL and GLES passed (%s)\n", glGetString(GL_RENDERER));
        status = 0;
    }
    goto done;

fail:
    fprintf(stderr, "EGL could not %s (error %#x)\n", stage, (unsigned)eglGetError());
done:
    if (display != EGL_NO_DISPLAY) {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
        if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
        eglTerminate(display);
    }
    return status;
}
