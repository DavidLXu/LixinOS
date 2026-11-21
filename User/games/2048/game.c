/* LixinOS window frontend for 2048.c. Tiles store base-two exponents. */
#ifdef __TINYC__
#include "/src/desktop/window-app.h"
#else
#include "../../desktop/window-app.h"
#endif
#include "board.h"
#include <stdio.h>
#include <string.h>

static uint8_t board[SIZE][SIZE];
static uint32_t score, seed = 1;
static int reached_goal, ended, held, start_x, start_y;
static const unsigned short paper = 0xff7b, ink = 0x4207;
static const unsigned short tiles[] = {
    0xce18, 0xff59, 0xff12, 0xfce8, 0xfbca, 0xf30a, 0xd9e7,
    0xef49, 0xeea7, 0xe627, 0xdda4, 0xcd03
};

static unsigned random_word(void) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

static void spawn_tile(void) {
    int empty = countEmpty(board);
    if (!empty) return;
    int chosen = random_word() % empty;
    for (int y = 0; y < SIZE; ++y)
        for (int x = 0; x < SIZE; ++x)
            if (!board[x][y] && chosen-- == 0) {
                board[x][y] = random_word() % 10 == 0 ? 2 : 1;
                return;
            }
}

static void new_game(void) {
    memset(board, 0, sizeof board);
    score = reached_goal = ended = 0;
    spawn_tile();
    spawn_tile();
}

static int move_tiles(int key) {
    if (ended) return 0;
    bool changed;
    switch (key) {
    case 300: case 'a': case 'A': changed = moveLeft(board, &score); break;
    case 301: case 'd': case 'D': changed = moveRight(board, &score); break;
    case 302: case 'w': case 'W': changed = moveUp(board, &score); break;
    case 303: case 's': case 'S': changed = moveDown(board, &score); break;
    default: return 0;
    }
    if (!changed) return 0;
    spawn_tile();
    ended = gameEnded(board);
    for (int x = 0; x < SIZE; ++x)
        for (int y = 0; y < SIZE; ++y)
            if (board[x][y] >= 11) reached_goal = 1;
    return 1;
}

static int event(AppWindow *s, WindowEvent e) {
    if (e.type == WINDOW_CLIENT_KEY) {
        if (e.value == 'q' || e.value == 27) return -1;
        if (e.value == 'r' || e.value == 'R') { new_game(); return 1; }
        return move_tiles(e.value);
    }
    if (e.type != WINDOW_CLIENT_POINTER) return 0;
    if (e.value && !held) { start_x = e.x; start_y = e.y; held = 1; }
    if (e.value || !held) return 0;
    held = 0;
    if (e.x < 0 || e.y < 0) return 0;
    if (start_x >= s->width-112 && start_y < 46 &&
        app_hit(e,s->width-112,8,100,34)) { new_game(); return 1; }
    int dx = e.x-start_x, dy = e.y-start_y;
    int ax = dx<0 ? -dx : dx, ay = dy<0 ? -dy : dy;
    if (start_y < 48 || (ax < 16 && ay < 16)) return 0;
    return move_tiles(ax > ay ? (dx < 0 ? 'a' : 'd') : (dy < 0 ? 'w' : 's'));
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

static void draw(AppWindow *s) {
    char heading[48];
    snprintf(heading,sizeof heading,"2048    Score %u",score);
    app_rect(s,0,0,s->width,s->height,paper);
    app_text(s,14,14,heading,ink);
    app_rect(s,s->width-112,8,100,34,0x3329);
    app_text(s,s->width-94,14,"New (R)",0xffff);
    int side = s->height-95;
    if (side > s->width-20) side = s->width-20;
    int cell = (side-5*5)/4;
    if (cell < 8) return;
    side = 4*cell+25;
    int left = (s->width-side)/2, top = 50+(s->height-95-side)/2;
    app_rect(s,left,top,side,side,0x9c91);
    for (int y = 0; y < SIZE; ++y)
        for (int x = 0; x < SIZE; ++x) {
            unsigned e = board[x][y];
            int px = left+5+x*(cell+5), py = top+5+y*(cell+5);
            app_rect(s,px,py,cell,cell,e<sizeof tiles/sizeof tiles[0] ? tiles[e] : 0x3329);
            if (e) tile_number(s,px,py,cell,1U<<e,e<3 ? ink : 0xffff);
        }
    const char *hint = ended ? "No moves left - R to restart" :
        reached_goal ? "2048 reached! Keep playing" : "Arrows / WASD / drag";
    int x = (s->width-(int)strlen(hint)*11)/2;
    app_text(s,x>0 ? x : 0,s->height-30,hint,ink);
}

int main(void) {
    if (lx_random(&seed,sizeof seed) < 0 || !seed) seed = (unsigned)lx_pid() | 1;
    new_game();
    return app_run3(event,draw);
}
