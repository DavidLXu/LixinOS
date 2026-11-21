#pragma once
#include <stdbool.h>
#include <SDL3/SDL.h>
#include "lua.h"

bool lxl_boot(int *width, int *height);
void lxl_bind(lua_State *L);
void lxl_damage(SDL_Surface *surface, const SDL_Rect *rects, int count);
void lxl_copy(const char *text);
int lxl_poll_input(lua_State *L);
