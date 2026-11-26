#include <lib/renderer.h>
#include <lib/window.h>
#include <SDL.h>

extern "C" void nfs_present(const void *pixels, unsigned w, unsigned h,
                            unsigned depth, const unsigned *palette);

namespace win32 {
Renderer::Renderer(WinApplication *app, Window *window)
    : m_application(app), m_window(window), m_renderer(nullptr), m_texture(0),
      m_videoMemory(new MemMap(800*600*2*2)), m_currentBuffer(0), m_width(0),
      m_height(0), m_depth(16), m_colorPalette{} {}
Renderer::~Renderer() { delete m_videoMemory; }

void Renderer::setVideoMode(x86::reg32 w, x86::reg32 h, x86::reg32 bpp) {
    NFS2_ASSERT(w && h && w <= 800 && h <= 600 && (bpp == 16 || bpp == 8));
    m_width = w;
    m_height = h;
    m_depth = bpp;
}
void Renderer::updatePalette(x86::reg32 count, const x86::reg32 *colors) {
    NFS2_ASSERT(count <= 256);
    for (unsigned i = 0; i < count; ++i) m_colorPalette[i] = colors[i] >> 8;
}
x86::reg32 Renderer::lock(x86::reg32 index) {
    return m_videoMemory->getBlockStart() + (m_currentBuffer ^ (index != 0)) * m_width * m_height * 2;
}
void Renderer::update() {
    if (m_width && m_height)
        nfs_present(&m_application->getMemory<x86::reg8>(lock(0)), m_width, m_height, m_depth, m_colorPalette);
}
void Renderer::unlock(x86::reg32 index) { if (!index) update(); }
void Renderer::swap() {
    m_currentBuffer ^= 1;
    update();
}
}
