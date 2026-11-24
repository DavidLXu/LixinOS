/* Expectimax: maximize over legal moves and average over every possible spawn.
 * Evaluation favors space and monotone rows/columns, weighted towards large tiles. */
#pragma once
#include "board.h"
#include <string.h>

static int line_value(int a, int b, int c, int d) {
    static int cache[65536];
    int key = 0;
    if ((a|b|c|d) < 16) {
        key = a | b<<4 | c<<8 | d<<12;
        if (cache[key]) return cache[key];
    }
    int values[4] = {a,b,c,d}, rises = 0, falls = 0, previous = 0, equal = 0, space = 0;
    for (int i = 0; i < SIZE; ++i) {
        int v = values[i];
        if (v == 0) ++space;
        else {
            if (v == previous) ++equal;
            previous = v;
        }
        if (i) {
            int u = values[i-1];
            int delta = v*v*v - u*u*u;
            if (delta > 0) rises += delta;
            else falls -= delta;
        }
    }
    int result = 320*space + 170*equal - 5*(rises < falls ? rises : falls);
    if ((a|b|c|d) < 16) cache[key] = result;
    return result;
}

static int evaluate(Board b) {
    int value = 0;
    for (int i = 0; i < SIZE; ++i) {
        value += line_value(b[i][0],b[i][1],b[i][2],b[i][3]);
        value += line_value(b[0][i],b[1][i],b[2][i],b[3][i]);
    }
    return value;
}

static int best_move(Board board, int depth, enum Direction *chosen);

static int after_spawn(Board board, int depth) {
    long long weighted = 0;
    int empty = 0;
    for (int x = 0; x < SIZE; ++x)
        for (int y = 0; y < SIZE; ++y) if (!board[x][y]) {
            ++empty;
            board[x][y] = 1;
            weighted += 9LL * best_move(board,depth,0);
            board[x][y] = 2;
            weighted += best_move(board,depth,0);
            board[x][y] = 0;
        }
    return empty ? (int)(weighted/(10*empty)) : best_move(board,depth,0);
}

static int best_move(Board board, int depth, enum Direction *chosen) {
    int best = -1000000;  /* Value of an actual game over. */
    enum Direction direction = NO_MOVE;
    for (int dir = LEFT; dir <= DOWN; ++dir) {
        Board next;
        uint32_t gain = 0;
        memcpy(next,board,sizeof next);
        if (!slideBoard(next,dir,&gain,0)) continue;
        int value = depth == 1 ? evaluate(next) : after_spawn(next,depth-1);
        if (direction == NO_MOVE || value > best) {
            best = value;
            direction = dir;
        }
    }
    /* Even when every branch ends in a loss, play a legal move if one exists. */
    if (chosen) *chosen = direction;
    return best;
}

static enum Direction auto_move(Board board) {
    enum Direction dir = NO_MOVE;
    best_move(board,countEmpty(board) <= 5 ? 3 : 2,&dir);
    return dir;
}
