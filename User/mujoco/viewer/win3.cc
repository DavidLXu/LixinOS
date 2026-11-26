#include "win3.h"
#include "simulate.h"
#include "window-client.h"
#include "render/classic/glad/glad.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

extern "C" void lx_allow_extended_heap();
extern "C" int win3_receive(void*, unsigned);
extern "C" int win3_send(const void*, unsigned);
extern "C" int mjGladLoadGL() {
    static const int loaded = mjGladLoadGLUnsafe(reinterpret_cast<void*>(eglGetProcAddress));
    return loaded;
}

namespace {
class Win3Adapter final : public mujoco::PlatformUIAdapter {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLConfig config{};
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    int width = 0, height = 0, x = 0, y = 0, capabilities = 0;
    bool closed = false, first = true, left = false, right = false, middle = false;
    bool alt = false, shift = false, control = false, vsync = true, accelerated = false;
    bool opening = false, fullscreen_request = false, pan = false, full_frame = true;
    std::string path, clipboard, escape, title;
    std::vector<unsigned char> rgb;
    std::vector<unsigned short> pixels, runs;

    void resize(WindowEvent e) {
        if (e.x < 1 || e.y < 1 || e.x > 2048 || e.y > 1152)
            throw std::runtime_error("invalid Win3 framebuffer size");
        capabilities = e.value;
        full_frame = true;
        if (width == e.x && height == e.y) return;
        const EGLint attrs[] = {EGL_WIDTH,e.x,EGL_HEIGHT,e.y,EGL_NONE};
        EGLSurface next = eglCreatePbufferSurface(display,config,attrs);
        if (!next || !eglMakeCurrent(display,next,next,context))
            throw std::runtime_error("could not allocate Win3 OpenGL surface");
        if (surface) eglDestroySurface(display,surface);
        surface = next;
        width = e.x;
        height = e.y;
        pixels.resize(width * height);
        rgb.resize(3 * pixels.size());
        runs.reserve(pixels.size());
    }

    void key(int code) {
        if (opening) {
            if (code == mjKEY_ENTER && !path.empty()) {
                const char *file = path.c_str();
                OnFilesDrop(1, &file);
                opening = false;
            } else if (code == mjKEY_ESCAPE) opening = false;
            else if (code == mjKEY_BACKSPACE && !path.empty()) path.pop_back();
            else if (code >= 32 && code <= 126 && path.size() < 999) path += char(code);
            return;
        }
        // Win3 reports right clicks, but older hosts cannot report right drags.
        // Ctrl+B toggles panning with the left button on those hosts.
        if (code == 2 && !(capabilities & WINDOW_CLIENT_RAW_BUTTONS_AVAILABLE)) { pan = !pan; return; }
        if (code == 15) { open(); return; }
        if (code > 0 && code < 27) { control = true; code += 'A' - 1; }
        if (code == '+' && !control) { code='='; shift=false; }
        if (code >= 'A' && code <= 'Z' && !control) shift = true;
        OnKey(code, 0, 1);
        alt = control = shift = false;
    }

    void keybyte(int byte) {
        if (!escape.empty()) {
            escape += char(byte);
            if (escape == "\033[" || escape == "\033O") return;
            if (escape.size() > 2 && (std::isdigit(byte) || byte == ';') && escape.size() < 10) return;
            struct Binding { const char *sequence; int key; };
            static constexpr Binding bindings[] = {
                {"[A",mjKEY_UP},{"[B",mjKEY_DOWN},{"[C",mjKEY_RIGHT},{"[D",mjKEY_LEFT},
                {"[H",mjKEY_HOME},{"[F",mjKEY_END},{"[5~",mjKEY_PAGE_UP},{"[6~",mjKEY_PAGE_DOWN},
                {"[3~",mjKEY_DELETE},{"[Z",mjKEY_TAB},{"OP",mjKEY_F1},{"OQ",mjKEY_F2},
                {"OR",mjKEY_F3},{"OS",mjKEY_F4},{"[15~",mjKEY_F5},{"[17~",mjKEY_F6},
                {"[18~",mjKEY_F7},{"[19~",mjKEY_F8},{"[20~",mjKEY_F9},{"[21~",mjKEY_F10},
                {"[23~",mjKEY_F11},{"[24~",mjKEY_F12}
            };
            for (auto binding : bindings) if (escape.substr(1) == binding.sequence) {
                shift |= escape == "\033[Z";
                key(binding.key);
                break;
            }
            escape.clear();
        } else if (control && byte > 0 && byte <= 26 && !opening) key(byte);
        else if (byte == 27) escape = "\033";
        else if (byte == 8 || byte == 127) key(mjKEY_BACKSPACE);
        else if (byte == 9) key(mjKEY_TAB);
        else if (byte == 13 || byte == 10) key(mjKEY_ENTER);
        else key(byte);
    }

public:
    Win3Adapter() {
        lx_allow_extended_heap();
        WindowEvent e{};
        if (!win3_receive(&e,sizeof e) || e.type != WINDOW_CLIENT_CONFIGURE)
            throw std::runtime_error("launch MuJoCo from Desktop");
        display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA,EGL_DEFAULT_DISPLAY,nullptr);
        const EGLint attrs[] = {EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_BIT,
            EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,
            EGL_DEPTH_SIZE,16,EGL_NONE};
        EGLint count = 0;
        if (!display || !eglInitialize(display,nullptr,nullptr) || !eglBindAPI(EGL_OPENGL_API)
            || !eglChooseConfig(display,attrs,&config,1,&count) || count != 1)
            throw std::runtime_error("Mesa desktop OpenGL configuration unavailable");
        context = eglCreateContext(display,config,EGL_NO_CONTEXT,nullptr);
        if (!context) throw std::runtime_error("Mesa compatibility context unavailable");
        resize(e);
        if (!mjGladLoadGL()) throw std::runtime_error("MuJoCo OpenGL entry points unavailable");
        const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        accelerated = renderer && std::strstr(renderer,"virgl");
        std::fprintf(stderr,"MuJoCo OpenGL: %s\n",renderer ? renderer : "unknown");
    }
    ~Win3Adapter() override {
        FreeMjrContext();
        eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
        eglDestroySurface(display,surface);
        eglDestroyContext(display,context);
        eglTerminate(display);
    }
    bool RefreshMjrContext(const mjModel* model, int scale) override {
        if (model == last_model_ && scale == last_fontscale_) return false;
        if (model) {
            // Configure rendering resources separately from the editable model.
            // Both guest drivers support single-sample RGBA8 framebuffers.
            mjModel render_model = *model;
            render_model.vis.quality.offsamples = 0;
            render_model.vis.quality.shadowsize = std::min(1024,model->vis.quality.shadowsize);
            render_model.vis.global.offwidth = std::min(model->vis.global.offwidth,std::max(800,width));
            render_model.vis.global.offheight = std::min(model->vis.global.offheight,std::max(600,height));
            mjr_makeContext(&render_model,&con_,scale);
        } else mjr_makeContext(nullptr,&con_,scale);
        last_model_ = model;
        last_fontscale_ = scale;
        return true;
    }
    void open() { opening = true; path.clear(); }
    std::pair<double,double> GetCursorPosition() const override { return {x,y}; }
    double GetDisplayPixelsPerInch() const override { return 96; }
    std::pair<int,int> GetFramebufferSize() const override { return {width,height}; }
    std::pair<int,int> GetWindowSize() const override { return {width,height}; }
    bool IsGPUAccelerated() const override { return accelerated; }
    bool ShouldCloseWindow() const override { return closed; }
    bool IsLeftMouseButtonPressed() const override { return left; }
    bool IsRightMouseButtonPressed() const override { return right; }
    bool IsMiddleMouseButtonPressed() const override { return middle; }
    bool IsAltKeyPressed() const override { return alt; }
    bool IsCtrlKeyPressed() const override { return control; }
    bool IsShiftKeyPressed() const override { return shift; }
    bool IsMouseButtonDownEvent(int act) const override { return act != 0; }
    bool IsKeyDownEvent(int act) const override { return act != 0; }
    int TranslateKeyCode(int code) const override {
        return code >= 'a' && code <= 'z' ? code - 'a' + 'A' : code;
    }
    mjtButton TranslateMouseButton(int button) const override { return static_cast<mjtButton>(button); }
    void SetClipboardString(const char* text) override { clipboard.assign(text, strnlen(text,WINDOW_CLIENT_CLIPBOARD_MAX)); }
    void SetVSync(bool enabled) override { vsync = enabled; }
    void SetWindowTitle(const char* text) override { title.assign(text,strnlen(text,199)); }
    void ToggleFullscreen() override { fullscreen_request = true; }

    void PollEvents() override {
        if (first) { first = false; OnWindowResize(width,height); return; }
        WindowEvent e{};
        if (!win3_receive(&e,sizeof e) || e.type == WINDOW_CLIENT_CLOSE) { closed = true; return; }
        if (e.type == WINDOW_CLIENT_CONFIGURE) { resize(e); OnWindowResize(width,height); }
        else if (e.type == WINDOW_CLIENT_POINTER) {
            x = e.x; y = e.y;
            shift = e.value & 2; control = e.value & 4; alt = e.value & 8;
            bool l = (e.value & 1) && !pan;
            bool r = (e.value & 16) || ((e.value & 1) && pan);
            bool m = e.value & 32;
            bool changed = l!=left || r!=right || m!=middle;
            if (l!=left) { left=l; OnMouseButton(mjBUTTON_LEFT,l); }
            if (r!=right) { right=r; OnMouseButton(mjBUTTON_RIGHT,r); }
            if (m!=middle) { middle=m; OnMouseButton(mjBUTTON_MIDDLE,m); }
            if (!changed && (l || r || m)) OnMouseMove(x,y);
        } else if (e.type == WINDOW_CLIENT_RIGHT_CLICK) {
            x = e.x; y = e.y; right = true; OnMouseButton(mjBUTTON_RIGHT,1);
            right = false; OnMouseButton(mjBUTTON_RIGHT,0);
        } else if (e.type == WINDOW_CLIENT_WHEEL) { x = e.x; y = e.y; OnScroll(0,e.value); }
        else if (e.type == WINDOW_CLIENT_KEY) { shift=e.x&1; control=e.x&2; alt=e.x&4; keybyte(e.value); }
        else if (e.type == WINDOW_CLIENT_TIMER && escape == "\033") { escape.clear(); key(mjKEY_ESCAPE); }
        else if (e.type == WINDOW_CLIENT_FILE_DROP) {
            char file[513];
            if (e.value<=0 || e.value>=int(sizeof file) || !win3_receive(file,e.value)) {
                closed=true; return;
            }
            file[e.value]=0;
            const char* files[]={file};
            opening=false;
            OnFilesDrop(1,files);
        } else if (e.type == WINDOW_CLIENT_PASTE) {
            char text[WINDOW_CLIENT_CLIPBOARD_MAX];
            if (e.value < 1 || e.value > WINDOW_CLIENT_CLIPBOARD_MAX || !win3_receive(text,e.value)) {
                closed = true; return;
            }
            if (opening) path.append(text,std::min<size_t>(e.value,999-path.size()));
            else {
                const auto* sim = static_cast<const mujoco::Simulate*>(state_.userdata);
                if (sim && (sim->ui0.editsect || sim->ui1.editsect)) {
                    control = alt = false;
                    for (int i=0; i<e.value && i<mjMAXUITEXT; ++i) {
                        unsigned char ch=text[i];
                        if (ch>=32 && ch<=126) { shift=std::isupper(ch); key(ch); }
                    }
                }
            }
        }
    }

    void SwapBuffers() override {
        if (closed) return;
        if (opening) mjr_overlay(mjFONT_NORMAL,mjGRID_TOP,{0,0,width,height},
            "Open model   |   Enter to load   |   Esc to cancel",path.c_str(),&con_);
        mjr_readPixels(rgb.data(),nullptr,{0,0,width,height},&con_);
        int top=height, bottom=0;
        for (int row=0; row<height; ++row) {
            const unsigned char* src = rgb.data()+3*(height-1-row)*width;
            bool changed=full_frame;
            for (int col=0; col<width; ++col,src+=3) {
                unsigned short value=(src[0]>>3)<<11 | (src[1]>>2)<<5 | (src[2]>>3);
                auto& pixel=pixels[row*width+col];
                changed |= pixel!=value;
                pixel=value;
            }
            if (changed) { top=std::min(top,row); bottom=row+1; }
        }
        full_frame=false;
        if (bottom==0) top=0;
        const size_t start=size_t(top)*width, end=size_t(bottom)*width;
        runs.clear();
        for (size_t i=start; i<end;) {
            const size_t first=i++;
            while (i<end && i-first<65535 && pixels[i]==pixels[first]) ++i;
            runs.push_back(i-first); runs.push_back(pixels[first]);
            if (runs.size()>=end-start) { runs.clear(); break; }
        }
        unsigned timer = vsync ? 32 : 16;
        if (capabilities & WINDOW_CLIENT_RIGHT_CLICK_AVAILABLE) timer |= WINDOW_CLIENT_WANT_RIGHT_CLICK;
        bool send_title = (capabilities & WINDOW_CLIENT_VIEWER_AVAILABLE) && !title.empty() && clipboard.empty();
        std::string& message = send_title ? title : clipboard;
        WindowReply reply{{0,(unsigned short)top,(unsigned short)(bottom ? width : 0),
            (unsigned short)(bottom-top),(unsigned short)timer},
            (unsigned short)message.size(),(unsigned)(2*runs.size())};
        if (capabilities & WINDOW_CLIENT_VIEWER_AVAILABLE) {
            reply.encoded_size |= WINDOW_CLIENT_WANT_MODIFIERS;
            if (capabilities & WINDOW_CLIENT_FILE_DROP_AVAILABLE) reply.encoded_size |= WINDOW_CLIENT_ACCEPT_FILES;
            if (send_title) reply.encoded_size |= WINDOW_CLIENT_WINDOW_TITLE;
            if (fullscreen_request) reply.encoded_size |= WINDOW_CLIENT_FULLSCREEN;
        }
        fullscreen_request = false;
        const auto* data = runs.empty() ? pixels.data()+start : runs.data();
        const size_t bytes = 2*(runs.empty() ? end-start : runs.size());
        closed = !win3_send(&reply,sizeof reply) || !win3_send(data,bytes)
              || (!message.empty() && !win3_send(message.data(),message.size()));
        message.clear();
        std::this_thread::yield();  // guest pthreads cooperate between frames
    }
};
}

std::unique_ptr<mujoco::PlatformUIAdapter> MakeWin3Adapter() {
    return std::make_unique<Win3Adapter>();
}
void OpenWin3Model(mujoco::PlatformUIAdapter& adapter) { static_cast<Win3Adapter&>(adapter).open(); }
