/* Movement and end-of-game rules from mevdschee/2048.c 1.0.3.
 * Copyright (c) 2024 Maurits van der Schee. MIT license (see LICENSE). */
#pragma once
#ifdef __TINYC__
typedef unsigned char uint8_t;
typedef unsigned int uint32_t;
typedef _Bool bool;
#define true 1
#define false 0
#else
#include <stdint.h>
#include <stdbool.h>
#endif
#define SIZE 4

uint8_t findTarget(uint8_t array[SIZE], uint8_t x, uint8_t stop)
{
	uint8_t t;
	// if the position is already on the first, don't evaluate
	if (x == 0)
	{
		return x;
	}
	for (t = x - 1;; t--)
	{
		if (array[t] != 0)
		{
			if (array[t] != array[x])
			{
				// merge is not possible, take next position
				return t + 1;
			}
			return t;
		}
		else
		{
			// we should not slide further, return this one
			if (t == stop)
			{
				return t;
			}
		}
	}
	// we did not find a target
	return x;
}

static bool slideLine(uint8_t array[SIZE], uint32_t *score, uint8_t *targets)
{
	bool success = false;
	uint8_t x, t, stop = 0;

	for (x = 0; x < SIZE; x++)
	{
		if (array[x] != 0)
		{
			t = findTarget(array, x, stop);
			if (targets) targets[x] = t;
			// if target is not original position, then move or merge
			if (t != x)
			{
				// if target is zero, this is a move
				if (array[t] == 0)
				{
					array[t] = array[x];
				}
				else if (array[t] == array[x])
				{
					// merge (increase power of two)
					array[t]++;
					// increase score
					*score += 1U << array[t];
					// set stop to avoid double merge
					stop = t + 1;
				}
				array[x] = 0;
				success = true;
			}
		}
	}
	return success;
}

enum Direction { LEFT, RIGHT, UP, DOWN, NO_MOVE };
typedef uint8_t Board[SIZE][SIZE];

/* Positions are numbered x*SIZE+y. Each original tile has one destination,
 * including both tiles participating in a merge. */
static int position(enum Direction d, int line, int step) {
    if (d == RIGHT || d == DOWN) step = SIZE-1-step;
    return (d == UP || d == DOWN) ? line*SIZE+step : step*SIZE+line;
}

static bool slideBoard(Board b, enum Direction d, uint32_t *score, uint8_t *dest) {
    bool changed = false;
    for (int line = 0; line < SIZE; ++line) {
        uint8_t values[SIZE], targets[SIZE] = {0};
        for (int i = 0; i < SIZE; ++i) {
            int p = position(d,line,i);
            values[i] = b[p/SIZE][p%SIZE];
        }
        changed |= slideLine(values, score, dest ? targets : 0);
        for (int i = 0; i < SIZE; ++i) {
            int p = position(d,line,i);
            b[p/SIZE][p%SIZE] = values[i];
            if (dest) dest[p] = position(d,line,targets[i]);
        }
    }
    return changed;
}

uint8_t countEmpty(uint8_t board[SIZE][SIZE])
{
	uint8_t x, y;
	uint8_t count = 0;
	for (x = 0; x < SIZE; x++)
	{
		for (y = 0; y < SIZE; y++)
		{
			if (board[x][y] == 0)
			{
				count++;
			}
		}
	}
	return count;
}

static bool gameEnded(Board b) {
    for (int x = 0; x < SIZE; ++x)
        for (int y = 0; y < SIZE; ++y)
            if (!b[x][y] || (x && b[x][y] == b[x-1][y]) || (y && b[x][y] == b[x][y-1]))
                return false;
    return true;
}
