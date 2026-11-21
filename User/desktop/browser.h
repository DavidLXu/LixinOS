#pragma once
int desk_web_open(unsigned *pixels, int stride, int height, const char *cwd, int width, int rows);
void desk_web_close(void);
const char *desk_web_title(void);
void desk_web_draw(int x, int y, int width, int height);
int desk_web_tick(void);
int desk_web_read_key(unsigned timeout_ms);
int desk_web_key(int key);
int desk_web_pointer(int x, int y, int down, int wheel);
void desk_web_cancel_pointer(void);
