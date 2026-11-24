/* Host regression checks for merge animation paths and forced-loss decisions. */
#include "solver.h"
#include <assert.h>

int main(void) {
    for (int dir = LEFT; dir <= DOWN; ++dir) {
        Board board = {0};
        uint8_t destinations[SIZE*SIZE];
        uint32_t score = 0;
        for (int step = 0; step < SIZE; ++step) {
            int p = position(dir,1,step);
            board[p/SIZE][p%SIZE] = 1;
        }
        assert(slideBoard(board,dir,&score,destinations) && score == 8);
        for (int step = 0; step < SIZE; ++step) {
            int p = position(dir,1,step);
            assert(destinations[p] == position(dir,1,step/2));
            assert(board[p/SIZE][p%SIZE] == (step < 2 ? 2 : 0));
        }
    }
    /* The only pair can merge, but every possible continuation loses. */
    Board forced = {{1,2,5,4},{4,8,9,11},{2,6,10,1},{1,4,9,1}}, original;
    memcpy(original,forced,sizeof forced);
    enum Direction legal = auto_move(forced);
    assert(legal != NO_MOVE && !memcmp(original,forced,sizeof forced));
    uint32_t score = 0;
    assert(slideBoard(forced,legal,&score,0) && score == 4);
    Board blocked = {{1,2,1,2},{2,1,2,1},{1,2,1,2},{2,1,2,1}};
    assert(gameEnded(blocked) && auto_move(blocked) == NO_MOVE);
}
