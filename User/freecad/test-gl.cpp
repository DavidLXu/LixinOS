// Exercise the compatibility-profile drawing calls used by the upstream CAD viewer.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glu.h>
#include <cstdio>

#ifdef TEST_COIN
bool test_scene();
#endif

int main()
{
    EGLDisplay display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, nullptr, nullptr);
    EGLConfig config;
    EGLint count = 0;
    const EGLint attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
    if (!eglInitialize(display, nullptr, nullptr) || !eglBindAPI(EGL_OPENGL_API) ||
        !eglChooseConfig(display, attrs, &config, 1, &count) || !count) {
        std::fprintf(stderr, "No desktop GL configuration (EGL %#x)\n", eglGetError());
        return 1;
    }
    const EGLint size[] = {EGL_WIDTH, 32, EGL_HEIGHT, 32, EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(display, config, size);
    EGLContext ctx = eglCreateContext(display, config, EGL_NO_CONTEXT, nullptr);
    if (!eglMakeCurrent(display, surface, surface, ctx) || ctx == EGL_NO_CONTEXT) {
        std::fprintf(stderr, "No desktop GL context (EGL %#x)\n", eglGetError());
        return 1;
    }
    glViewport(0, 0, 32, 32);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluOrtho2D(-1, 1, -1, 1);
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glColor3f(1, 0, 0);
    glBegin(GL_TRIANGLES);
    glVertex2f(-0.8f, -0.8f);
    glVertex2f(0.8f, -0.8f);
    glVertex2f(0, 0.8f);
    glEnd();
    unsigned char center[4] = {}, corner[4] = {};
    glReadPixels(16, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, center);
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, corner);
    bool passed = glGetError() == GL_NO_ERROR && center[0] == 255 && center[2] == 0 &&
                  corner[0] == 0 && corner[2] == 255;
    std::printf("desktop GL %s (%s), GLU %s, triangle readback %s\n", glGetString(GL_VERSION),
                glGetString(GL_RENDERER), gluGetString(GLU_VERSION), passed ? "passed" : "failed");
#ifdef TEST_COIN
    passed = passed && test_scene();
#endif
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, ctx);
    eglDestroySurface(display, surface);
    eglTerminate(display);
    return passed ? 0 : 1;
}
