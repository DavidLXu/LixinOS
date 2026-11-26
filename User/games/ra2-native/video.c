// Feed SDL's software renderer and event state from a foreground LixinOS canvas.
#include "SDL.h"
// SDL2's internal send APIs update the polling state and enqueue the event.
extern void SDL_SetKeyboardFocus(SDL_Window *);
extern void SDL_SetMouseFocus(SDL_Window *);
extern int SDL_SendMouseMotion(SDL_Window *, Uint32, int, int, int);
extern int SDL_SendMouseButton(SDL_Window *, Uint32, Uint8, Uint8);
extern int SDL_SendMouseWheel(SDL_Window *, Uint32, float, float, SDL_MouseWheelDirection);
extern int SDL_SendKeyboardKey(Uint8, SDL_Scancode);
extern int SDL_SendKeyboardText(const char *);
#include "lixin-gui.h"
#include "../../console.h"
#include <string.h>
static struct lx_canvas canvas;
static int window_w, window_h, view_w, view_h, view_x, view_y, last_down;
static struct lx_pointer pointer;
extern int __real_SDL_Init(Uint32);
extern int __real_SDL_InitSubSystem(Uint32);
extern SDL_Renderer *__real_SDL_CreateRenderer(SDL_Window *,int,Uint32);
extern void __real_SDL_RenderPresent(SDL_Renderer *);
extern void __real_SDL_PumpEvents(void);
extern int __real_SDL_PollEvent(SDL_Event *);
static void prepare_sdl(Uint32 flags) {
    SDL_setenv("SDL_VIDEODRIVER","dummy",1);
    SDL_setenv("SDL_AUDIODRIVER","dummy",1);
    SDL_SetHint(SDL_HINT_RENDER_DRIVER,"software");
    if((flags & SDL_INIT_VIDEO) && !canvas.pixels) lx_gui_open(&canvas);
}
int __wrap_SDL_Init(Uint32 flags) {
    prepare_sdl(flags);
    return __real_SDL_Init(flags);
}
int __wrap_SDL_InitSubSystem(Uint32 flags) {
    prepare_sdl(flags);
    return __real_SDL_InitSubSystem(flags);
}
extern SDL_Window *__real_SDL_CreateWindow(const char *,int,int,int,int,Uint32);
SDL_Window *__wrap_SDL_CreateWindow(const char *name,int x,int y,int w,int h,Uint32 flags) {
    SDL_Window *win=__real_SDL_CreateWindow(name,x,y,w,h,flags);
    if(win) {SDL_SetKeyboardFocus(win);SDL_SetMouseFocus(win);}
    return win;
}
SDL_Renderer *__wrap_SDL_CreateRenderer(SDL_Window *window,int index,Uint32 flags) {
    return __real_SDL_CreateRenderer(window,index,(flags & ~(SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC)) | SDL_RENDERER_SOFTWARE);
}
static void input(void) {
    SDL_Window *win=SDL_GetKeyboardFocus();
    if(!win || !canvas.pixels || !window_w || !window_h) return;
    for(int i=0;i<32;i++) {
        long event=console_call(21,8); // raw canvas events, including right-click
        if(!lx_gui_decode_pointer(&pointer,event)) break;
        int x=(int)pointer.x-view_x, y=(int)pointer.y-view_y;
        int inside=pointer.inside && !pointer.cancelled && x>=0 && x<view_w && y>=0 && y<view_h;
        if(inside) SDL_SendMouseMotion(win,0,0,x*window_w/view_w,y*window_h/view_h);
        int down=inside && pointer.down;
        if(down!=last_down) SDL_SendMouseButton(win,0,down?SDL_PRESSED:SDL_RELEASED,SDL_BUTTON_LEFT);
        last_down=down;
        if(inside && (event & (1L<<29))) {
            SDL_SendMouseButton(win,0,SDL_PRESSED,SDL_BUTTON_RIGHT);
            SDL_SendMouseButton(win,0,SDL_RELEASED,SDL_BUTTON_RIGHT);
        }
        if(inside && pointer.wheel) SDL_SendMouseWheel(win,0,0,pointer.wheel,SDL_MOUSEWHEEL_NORMAL);
    }
    int key=lx_timed_byte(0);
    if(key<0) return;
    if(key==17) {SDL_Event e={.type=SDL_QUIT}; SDL_PushEvent(&e);return;}
    if(key==27 && lx_timed_byte(10)=='[') {
        int arrow=lx_timed_byte(10);
        if(arrow>='A' && arrow<='D') {
            SDL_Keycode codes[]={SDLK_UP,SDLK_DOWN,SDLK_RIGHT,SDLK_LEFT};
            key=codes[arrow-'A'];
        }
    }
    if(key=='\n') key=SDLK_RETURN;
    if(key==127) key=SDLK_BACKSPACE;
    SDL_Scancode code=SDL_GetScancodeFromKey(key);
    SDL_SendKeyboardKey(SDL_PRESSED,code);
    if(key>=32 && key<127 && SDL_IsTextInputActive()) {
        char ascii[2]={(char)key,0};SDL_SendKeyboardText(ascii);
    }
    SDL_SendKeyboardKey(SDL_RELEASED,code);
}
void __wrap_SDL_PumpEvents(void) {input();__real_SDL_PumpEvents();}
int __wrap_SDL_PollEvent(SDL_Event *event) {input();return __real_SDL_PollEvent(event);}
static void present_window(SDL_Window *win) {
    if(!canvas.pixels) return;
    SDL_Surface *surface=win?SDL_GetWindowSurface(win):NULL;
    if(!surface || surface->format->format!=SDL_PIXELFORMAT_RGB888) return;
    if(window_w!=surface->w || window_h!=surface->h) {
        window_w=surface->w;window_h=surface->h;
        view_w=canvas.width;view_h=view_w*window_h/window_w;
        if(view_h>(int)canvas.height) {view_h=canvas.height;view_w=view_h*window_w/window_h;}
        view_x=((int)canvas.width-view_w)/2;view_y=((int)canvas.height-view_h)/2;
        lx_gui_rect(&canvas,0,0,canvas.width,canvas.height,0);
    }
    for(int y=0;y<view_h;y++) {
        const unsigned *row=(const unsigned*)((char*)surface->pixels+(y*window_h/view_h)*surface->pitch);
        unsigned *dest=canvas.pixels+(y+view_y)*canvas.width+view_x;
        if(view_w==window_w) memcpy(dest,row,view_w*sizeof(*row));
        else for(int x=0;x<view_w;x++) dest[x]=row[x*window_w/view_w];
    }
    lx_gui_present(&canvas);
    if(pointer.inside) lx_gui_cursor_tile(&canvas,pointer.x,pointer.y,1,0);
}

void __wrap_SDL_RenderPresent(SDL_Renderer *r) {
    __real_SDL_RenderPresent(r);
    present_window(SDL_RenderGetWindow(r));
}
extern int __real_SDL_UpdateWindowSurface(SDL_Window *);
int __wrap_SDL_UpdateWindowSurface(SDL_Window *win) {
    int result=__real_SDL_UpdateWindowSurface(win);
    if(result==0) present_window(win);
    return result;
}
