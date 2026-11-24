/* Host regression for online updates, deterministic resume, and frozen evaluation. */
#include "learn.h"
#include <assert.h>

static Learner learner, saved;
static EvalScore baseline[2];
static Evaluation run_eval(void) {
    Evaluation e;
    start_evaluation(&e);
    while (!eval_step(&e,&learner)) {}
    assert(e.result[0].games == EVAL_GAMES && e.result[1].games == EVAL_GAMES);
    return e;
}
int main(void) {
    start_episode(&learner.train,34723);
    learner.explore_rng = 195273;
    Evaluation initial = run_eval();
    assert(!memcmp(&initial.result[0],&initial.result[1],sizeof(EvalScore)));
    memcpy(baseline,initial.result,sizeof baseline);

    while (learner.games < 32) train_step(&learner);
    assert(learner.samples > 32 && learner.best && learner.history[31]);
    saved = learner;
    Evaluation a = run_eval(), b = run_eval();
    assert(!memcmp(&saved,&learner,sizeof learner));
    assert(!memcmp(a.result,b.result,sizeof a.result));
    assert(!memcmp(&a.result[1],&baseline[1],sizeof(EvalScore)));
    assert(a.result[0].total != a.result[1].total);
    for (int i = 0; i < 700; ++i) {
        train_step(&learner);
        train_step(&saved);
    }
    assert(!memcmp(&learner,&saved,sizeof learner));
}
