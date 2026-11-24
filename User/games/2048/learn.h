/* Online afterstate TD(0). A value describes the board after a slide, before
 * the random tile. Training and held-out evaluation have independent games. */
#pragma once
#include "board.h"
#include <string.h>

enum { RANKS = 12, PATTERNS = RANKS*RANKS*RANKS, FEATURES = 52,
       HISTORY_LEN = 48, EVAL_GAMES = 16, VALUE_SCALE = 256 };
typedef struct {
    Board board, after;
    uint32_t rng, score;
    int has_after;
} LearnGame;
typedef struct { uint32_t games, total, tile, wins; } EvalScore;
typedef struct {
    uint32_t version, digest;
    int weights[2*PATTERNS];
    LearnGame train;
    uint32_t explore_rng, samples, games, history[HISTORY_LEN], best;
    uint32_t evaluated_at;
    EvalScore result[2];
} Learner;
typedef struct {
    LearnGame game;
    EvalScore result[2];  /* Model, then zero-weight reference on identical seeds. */
    int reference;
} Evaluation;

static unsigned next_word(uint32_t *rng) {
    *rng ^= *rng << 13;
    *rng ^= *rng >> 17;
    *rng ^= *rng << 5;
    return *rng;
}
static int add_random_tile(Board b, uint32_t *rng) {
    int empty = countEmpty(b);
    if (!empty) return -1;
    int chosen = next_word(rng) % empty;
    for (int y = 0; y < SIZE; ++y)
        for (int x = 0; x < SIZE; ++x)
            if (!b[x][y] && chosen-- == 0) {
                b[x][y] = next_word(rng) % 10 == 0 ? 2 : 1;
                return x*SIZE+y;
            }
    return -1;
}
static void start_episode(LearnGame *g, uint32_t rng) {
    memset(g,0,sizeof *g);
    g->rng = rng ? rng : 1;
    add_random_tile(g->board,&g->rng);
    add_random_tile(g->board,&g->rng);
}
static int pattern(int a, int b, int c) {
    if (a >= RANKS) a = RANKS-1;
    if (b >= RANKS) b = RANKS-1;
    if (c >= RANKS) c = RANKS-1;
    if (a > c) { int swap = a; a = c; c = swap; }
    return (a*RANKS+b)*RANKS+c;
}
static void features(Board b, unsigned short ids[FEATURES]) {
    int n = 0;
    for (int i = 0; i < SIZE; ++i)
        for (int j = 0; j < SIZE-2; ++j) {
            ids[n++] = pattern(b[i][j],b[i][j+1],b[i][j+2]);
            ids[n++] = pattern(b[j][i],b[j+1][i],b[j+2][i]);
        }
    for (int x = 0; x < SIZE-1; ++x)
        for (int y = 0; y < SIZE-1; ++y) {
            int a = b[x][y], c = b[x+1][y], d = b[x+1][y+1], e = b[x][y+1];
            ids[n++] = PATTERNS + pattern(c,a,e);
            ids[n++] = PATTERNS + pattern(a,c,d);
            ids[n++] = PATTERNS + pattern(c,d,e);
            ids[n++] = PATTERNS + pattern(d,e,a);
        }
}
static int prediction(const int *weights, const unsigned short ids[FEATURES]) {
    long long sum = 0;
    for (int i = 0; i < FEATURES; ++i) sum += weights[ids[i]];
    return (int)(sum/FEATURES);
}
static void learn_target(Learner *m, Board after, int target) {
    unsigned short ids[FEATURES];
    static unsigned char counts[2*PATTERNS];
    features(after,ids);
    int error = target - prediction(m->weights,ids);
    int norm = 0;
    for (int i = 0; i < FEATURES; ++i) norm += 2*counts[ids[i]]++ + 1;
    /* Normalized semi-gradient, step size 1/16. Repeated patterns contribute
     * once per occurrence to both the prediction and its gradient. */
    int delta = (int)((long long)error*FEATURES/(16*norm));
    for (int i = 0; i < FEATURES; ++i) {
        m->weights[ids[i]] += delta;
        counts[ids[i]] = 0;
    }
}
static unsigned peak_tile(Board b) {
    int peak = 0;
    for (int x = 0; x < SIZE; ++x)
        for (int y = 0; y < SIZE; ++y)
            if (b[x][y] > peak) peak = b[x][y];
    return 1u << peak;
}

/* Sample a legal action (5% exploration in training only). Score each
 * candidate by its immediate merge reward plus discounted learned value. */
static void policy_step(LearnGame *g, const int *weights, Learner *training) {
    int explore = training && next_word(&training->explore_rng)%20 == 0;
    int legal = 0, best = 0;
    uint32_t reward = 0;
    Board chosen;
    for (int dir = LEFT; dir <= DOWN; ++dir) {
        Board candidate;
        uint32_t gain = 0;
        memcpy(candidate,g->board,sizeof candidate);
        if (!slideBoard(candidate,dir,&gain,0)) continue;
        int value = gain*VALUE_SCALE;
        if (weights) {
            unsigned short ids[FEATURES];
            features(candidate,ids);
            value += (int)((long long)prediction(weights,ids)*99/100);
        }
        ++legal;
        if (legal == 1 || (explore ? next_word(&training->explore_rng)%legal == 0 : value > best)) {
            best = value;
            reward = gain;
            memcpy(chosen,candidate,sizeof chosen);
        }
    }
    if (!legal) return;
    if (training) {
        if (g->has_after) learn_target(training,g->after,best);
        memcpy(g->after,chosen,sizeof chosen);
        g->has_after = 1;
        ++training->samples;
    }
    memcpy(g->board,chosen,sizeof chosen);
    g->score += reward;
    add_random_tile(g->board,&g->rng);
}
static void train_step(Learner *m) {
    LearnGame *g = &m->train;
    policy_step(g,m->weights,m);
    if (gameEnded(g->board)) {
        learn_target(m,g->after,0);
        m->history[m->games % HISTORY_LEN] = g->score;
        ++m->games;
        if (g->score > m->best) m->best = g->score;
        start_episode(g,g->rng);
    }
}
static uint32_t eval_seed(unsigned i) { return 0x92d68ca1u ^ (i*0x9e3779b9u); }
static void start_evaluation(Evaluation *e) {
    memset(e,0,sizeof *e);
    start_episode(&e->game,eval_seed(0));
}
static int eval_step(Evaluation *e, const Learner *m) {
    LearnGame *g = &e->game;
    policy_step(g,e->reference ? 0 : m->weights,0);
    if (!gameEnded(g->board)) return 0;
    EvalScore *score = &e->result[e->reference];
    unsigned tile = peak_tile(g->board);
    ++score->games;
    score->total += g->score;
    if (tile > score->tile) score->tile = tile;
    if (tile >= 2048) ++score->wins;
    if (score->games == EVAL_GAMES) {
        if (e->reference) return 1;
        e->reference = 1;
    }
    start_episode(g,eval_seed(e->result[e->reference].games));
    return 0;
}
