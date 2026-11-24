/* LixinOS window frontend for 2048.c. Tiles store base-two exponents. */
#ifdef __TINYC__
#include "/src/desktop/window-app.h"
#else
#include "../../desktop/window-app.h"
#endif
#include "solver.h"
#include "learn.h"
#include <stdio.h>
#include <string.h>

long lx_console_call(long, long);
enum { SLIDE_MS = 100, SETTLE_MS = 40, AUTO_PAUSE_MS = 80 };
static Board board, before;
static uint8_t destinations[SIZE*SIZE], arrivals[SIZE*SIZE];
static int spawned, animating, automatic, elapsed;
static long started, next_auto;
static enum Direction queued = NO_MOVE;
static uint32_t old_score;
static uint32_t score, seed = 1;
static int reached_goal, ended, held, start_x, start_y;
static const unsigned short paper = 0xff7b, ink = 0x4207;
static const unsigned short tiles[] = {
    0xce18, 0xff59, 0xff12, 0xfce8, 0xfbca, 0xf30a, 0xd9e7,
    0xef49, 0xeea7, 0xe627, 0xdda4, 0xcd03
};

#include "learn-panel.h"

static int spawn_tile(void) { return add_random_tile(board,&seed); }

static void open_lab(void) {
    load_model();
    automatic = animating = held = 0;
    queued = NO_MOVE;
    panel_open = 1;
}

static void new_game(void) {
    memset(board, 0, sizeof board);
    score = reached_goal = ended = animating = automatic = held = 0;
    queued = NO_MOVE;
    spawn_tile();
    spawn_tile();
}

static enum Direction key_direction(int key) {
    switch (key) {
    case 300: case 'a': case 'A': return LEFT;
    case 301: case 'd': case 'D': return RIGHT;
    case 302: case 'w': case 'W': return UP;
    case 303: case 's': case 'S': return DOWN;
    default: return NO_MOVE;
    }
}

static int move_tiles(enum Direction dir, long now) {
    if (ended || dir == NO_MOVE) return 0;
    memcpy(before,board,sizeof before);
    old_score = score;
    if (!slideBoard(board,dir,&score,destinations)) return 0;
    memset(arrivals,0,sizeof arrivals);
    for (int p = 0; p < SIZE*SIZE; ++p)
        if (before[p/SIZE][p%SIZE]) ++arrivals[destinations[p]];
    spawned = spawn_tile();
    ended = gameEnded(board);
    if (ended) automatic = 0;
    for (int x = 0; x < SIZE; ++x)
        for (int y = 0; y < SIZE; ++y)
            if (board[x][y] >= 11) reached_goal = 1;
    started = now;
    elapsed = 0;
    animating = 1;
    return 1;
}

static int manual_move(enum Direction dir, long now) {
    if (dir == NO_MOVE) return 0;
    int was_auto = automatic;
    automatic = 0;
    if (animating) queued = dir;
    else if (move_tiles(dir,now)) return 1;
    return was_auto;
}

static int tick(long now) {
    if (animating) {
        long age = now-started;
        if (age < SLIDE_MS+SETTLE_MS) {
            elapsed = (int)age;
            return 1;
        }
        animating = 0;
        enum Direction dir = queued;
        queued = NO_MOVE;
        if (dir != NO_MOVE) move_tiles(dir,now);
        next_auto = now + AUTO_PAUSE_MS;
        return 1;
    }
    if (automatic && now >= next_auto) {
        enum Direction dir = auto_move(board);
        if (dir != NO_MOVE && move_tiles(dir,lx_console_call(38,0))) return 1;
        automatic = 0;
        return 1;
    }
    return 0;
}

static int toggle_auto(long now) {
    if (ended) return 0;
    automatic = !automatic;
    queued = NO_MOVE;
    next_auto = now;
    return 1;
}

static int event(AppWindow *s, WindowEvent e) {
    int changed = 0;
    long now = lx_console_call(38,0);
    if (panel_open) return lab_event(s,e,now);
    if (e.type == WINDOW_CLIENT_TIMER) changed = tick(now);
    else if (e.type == WINDOW_CLIENT_KEY) {
        if (e.value == 'q' || e.value == 27) return -1;
        if (e.value == 'r' || e.value == 'R') { new_game(); changed = 1; }
        else if (e.value == 'p' || e.value == 'P') changed = toggle_auto(now);
        else if (e.value == 'l' || e.value == 'L') { open_lab(); changed = 1; }
        else changed = manual_move(key_direction(e.value),now);
    } else if (e.type == WINDOW_CLIENT_POINTER) {
        if (e.value && !held) { start_x = e.x; start_y = e.y; held = 1; }
        if (!e.value && held) {
            held = 0;
            WindowEvent press = e;
            press.x = start_x; press.y = start_y;
            int ax = e.x-start_x, ay = e.y-start_y;
            int dx = ax < 0 ? -ax : ax, dy = ay < 0 ? -ay : ay;
            if (app_hit(press,s->width-90,8,80,34) && app_hit(e,s->width-90,8,80,34)) {
                new_game(); changed = 1;
            } else if (app_hit(press,s->width-198,8,100,34) && app_hit(e,s->width-198,8,100,34))
                changed = toggle_auto(now);
            else if (app_hit(press,s->width-276,8,72,34) && app_hit(e,s->width-276,8,72,34)) {
                open_lab(); changed = 1;
            } else if (e.x >= 0 && e.y >= 0 && start_y >= 48 && (dx >= 16 || dy >= 16))
                changed = manual_move(dx > dy ? (ax < 0 ? LEFT : RIGHT) : (ay < 0 ? UP : DOWN),now);
        }
    }
    s->timer_ms = animating ? 16 : automatic ? AUTO_PAUSE_MS : 0;
    return changed;
}

/* Draw tile values with the same proportions at every window size. */
static void tile_number(AppWindow *s, int x, int y, int cell, unsigned value, unsigned short color) {
    static const unsigned char digits[10][7] = {
        {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14}, {14,17,1,2,4,8,31},
        {30,1,1,14,1,1,30}, {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
        {6,8,16,30,17,17,14}, {31,1,2,4,8,8,8}, {14,17,17,14,17,17,14},
        {14,17,17,15,1,2,12}
    };
    char text[12];
    int n = snprintf(text,sizeof text,"%u",value);
    int scale = (cell-6)/(6*n-1);
    if (scale > cell/14) scale = cell/14;
    if (scale < 1) return;
    x += (cell-(6*n-1)*scale)/2;
    y += (cell-7*scale)/2;
    for (int i = 0; i < n; ++i)
        for (int row = 0; row < 7; ++row)
            for (int col = 0; col < 5; ++col)
                if (digits[text[i]-'0'][row] & (16>>col))
                    app_rect(s,x+(6*i+col)*scale,y+row*scale,scale,scale,color);
}

static void draw_tile(AppWindow *s, int px, int py, int size, unsigned e) {
    app_rect(s,px,py,size,size,e<sizeof tiles/sizeof tiles[0] ? tiles[e] : 0x3329);
    if (e) tile_number(s,px,py,size,1U<<e,e<3 ? ink : 0xffff);
}

static void draw(AppWindow *s) {
    if (panel_open) { lab_draw(s); return; }
    char heading[48];
    unsigned visible_score = animating && elapsed < SLIDE_MS ? old_score : score;
    snprintf(heading,sizeof heading,s->width >= 516 ? "2048    Score %u" : "%u",visible_score);
    app_rect(s,0,0,s->width,s->height,paper);
    if (s->width >= 380) app_text(s,14,14,heading,ink);
    app_rect(s,s->width-276,8,72,34,0x43cd);
    app_text(s,s->width-268,14,"Learn",0xffff);
    app_rect(s,s->width-198,8,100,34,automatic ? 0xb4c4 : 0x43cd);
    app_text(s,s->width-192,14,automatic ? "Stop (P)" : "Auto (P)",0xffff);
    app_rect(s,s->width-90,8,80,34,0x3329);
    app_text(s,s->width-88,14,"New (R)",0xffff);
    int side = s->height-95;
    if (side > s->width-20) side = s->width-20;
    int cell = (side-5*5)/4;
    if (cell < 8) return;
    side = 4*cell+25;
    int left = (s->width-side)/2, top = 50+(s->height-95-side)/2;
    app_rect(s,left,top,side,side,0x9c91);
    int pitch = cell+5;
    for (int x = 0; x < SIZE; ++x)
        for (int y = 0; y < SIZE; ++y)
            draw_tile(s,left+5+x*pitch,top+5+y*pitch,cell,0);
    for (int p = 0; p < SIZE*SIZE; ++p) {
        int x = p/SIZE, y = p%SIZE, px = left+5+x*pitch, py = top+5+y*pitch;
        unsigned e = board[x][y];
        int inset = 0;
        if (animating && elapsed < SLIDE_MS) {
            e = before[x][y];
            int target = destinations[p];
            int travel = elapsed*(2*SLIDE_MS-elapsed);
            px += (target/SIZE-x)*pitch*travel/(SLIDE_MS*SLIDE_MS);
            py += (target%SIZE-y)*pitch*travel/(SLIDE_MS*SLIDE_MS);
        } else if (animating) {
            int remaining = SLIDE_MS+SETTLE_MS-elapsed;
            if (p == spawned) inset = cell*remaining/(5*SETTLE_MS);
            else if (arrivals[p] == 2) inset = -2*remaining/SETTLE_MS;
        }
        if (e) draw_tile(s,px+inset,py+inset,cell-2*inset,e);
    }
    const char *hint = ended ? "No moves left - R to restart" :
        automatic ? "Auto play - P to stop" :
        reached_goal ? "2048 reached! Keep playing" : "Arrows / WASD / drag";
    if (s->width < 380 && !ended) {
        snprintf(heading,sizeof heading,"Score %u   %s",visible_score,automatic ? "Auto" : "WASD / drag");
        hint = heading;
    }
    int x = (s->width-(int)strlen(hint)*11)/2;
    app_text(s,x>0 ? x : 0,s->height-30,hint,ink);
}

int main(void) {
    if (lx_random(&seed,sizeof seed) < 0 || !seed) seed = (unsigned)lx_pid() | 1;
    new_game();
    int status = app_run3(event,draw);
    save_model();
    return status;
}
