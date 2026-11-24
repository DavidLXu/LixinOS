/* Training runs in small chunks on window timers so controls remain live. */
enum LearnMode { PAUSED, TRAINING, EVALUATING };
static enum LearnMode learn_mode;
static Learner model;
static Evaluation evaluation;
static int panel_open, loaded, dirty, save_failed;
static uint32_t disk_digest;
static const char *checkpoint = "/home/lixin/.2048-learning";
typedef char model_fits_atomic_save[sizeof(Learner) <= 32768 ? 1 : -1];

static uint32_t model_digest(void) {
    const unsigned char *bytes = (const unsigned char *)&model;
    uint32_t hash = 2166136261u;
    for (unsigned i = 8; i < sizeof model; ++i) hash = (hash^bytes[i])*16777619u;
    return hash;
}
static int read_exact(int fd, void *out, int size) {
    int used = 0;
    while (used < size) {
        int n = lx_read(fd,(char *)out+used,size-used);
        if (n <= 0) return 0;
        used += n;
    }
    return 1;
}
static void load_model(void) {
    if (loaded) return;
    loaded = 1;
    if (lx_stat("/home/lixin") != LX_DIR) checkpoint = "/.2048-learning";
    long size = lx_stat(checkpoint);
    if (size == sizeof model) {
        int fd = lx_open(checkpoint);
        if (fd >= 0) {
            int ok = read_exact(fd,&model,sizeof model);
            lx_close(fd);
            if (ok && model.version == 1 && model.digest == model_digest() && model.train.rng) {
                disk_digest = model.digest;
                return;
            }
        }
    }
    memset(&model,0,sizeof model);
    model.version = 1;
    start_episode(&model.train,seed);
    model.explore_rng = seed^0xb23a7521u;
    if (!model.explore_rng) model.explore_rng = 1;
    if (size != LX_MISSING) save_failed = 2;
}
static void save_model(void) {
    if (!dirty || save_failed == 2) return;
    /* Detect a checkpoint replaced by another game before saving local progress. */
    long size = lx_stat(checkpoint);
    if (size != LX_MISSING) {
        uint32_t header[2];
        int fd = lx_open(checkpoint), ok = fd >= 0 && read_exact(fd,header,sizeof header);
        if (fd >= 0) lx_close(fd);
        if (!ok || size != sizeof model || header[0] != 1 || header[1] != disk_digest) {
            save_failed = 2;
            return;
        }
    } else if (disk_digest) { save_failed = 2; return; }
    model.digest = model_digest();
    save_failed = lx_save_file(checkpoint,&model,sizeof model) != 0;
    if (!save_failed) { dirty = 0; disk_digest = model.digest; }
}
static void lab_command(int command) {
    if (command == 'l') {
        learn_mode = PAUSED;
        save_model();
        panel_open = 0;
    } else if (command == 'e') {
        if (learn_mode == EVALUATING) learn_mode = PAUSED;
        else {
            save_model();
            start_evaluation(&evaluation);
            learn_mode = EVALUATING;
        }
    } else if (command == ' ') {
        if (learn_mode != PAUSED) {
            learn_mode = PAUSED;
            save_model();
        } else if (save_failed != 2) learn_mode = TRAINING;
    }
}
static int lab_tick(long now) {
    if (learn_mode == PAUSED) return 0;
    for (int i = 0; i < 96; ++i) {
        if (learn_mode == TRAINING) {
            uint32_t games = model.games;
            train_step(&model);
            dirty = 1;
            if (model.games != games && model.games%128 == 0) {
                save_model();
                if (save_failed) learn_mode = PAUSED;
                break;
            }
        } else if (eval_step(&evaluation,&model)) {
            memcpy(model.result,evaluation.result,sizeof model.result);
            model.evaluated_at = model.samples;
            dirty = 1;
            learn_mode = PAUSED;
            save_model();
            break;
        }
        if ((i&7) == 7 && lx_console_call(38,0)-now >= 6) break;
    }
    return 1;
}
static int lab_event(AppWindow *s, WindowEvent e, long now) {
    int changed = 0;
    if (e.type == WINDOW_CLIENT_TIMER) changed = lab_tick(now);
    if (e.type == WINDOW_CLIENT_KEY) {
        if (e.value == 'q' || e.value == 'Q') return -1;
        int key = e.value == 27 ? 'l' : e.value;
        if (key == 'L' || key == 'E') key += 'a'-'A';
        if (key == 'l' || key == 'e' || key == ' ') { lab_command(key); changed = 1; }
    } else if (e.type == WINDOW_CLIENT_POINTER) {
        if (e.value && !held) { start_x = e.x; start_y = e.y; held = 1; }
        if (!e.value && held) {
            held = 0;
            int width = (s->width-16)/3;
            WindowEvent press = e;
            press.x = start_x; press.y = start_y;
            for (int i = 0; i < 3; ++i)
                if (app_hit(press,8+i*width,8,width-4,30) && app_hit(e,8+i*width,8,width-4,30)) {
                    lab_command(" el"[i]); changed = 1;
                }
        }
    }
    s->timer_ms = panel_open && learn_mode != PAUSED ? 16 : 0;
    return changed;
}
static void chart_line(AppWindow *s, int x1, int y1, int x2, int y2, unsigned short color) {
    int dx = x2-x1, dy = y2-y1;
    int height = dy < 0 ? -dy : dy;
    int steps = dx > height ? dx : height;
    for (int i = 0; i <= steps; ++i)
        app_rect(s,x1+(steps ? dx*i/steps : 0),y1+(steps ? dy*i/steps : 0),2,2,color);
}
static void lab_draw(AppWindow *s) {
    char text[112];
    int w = s->width, h = s->height, button = (w-16)/3;
    app_rect(s,0,0,w,h,paper);
    const char *buttons[] = {learn_mode == PAUSED ? "Train" : "Pause",
                             learn_mode == EVALUATING ? "Cancel" : "Evaluate","Game"};
    for (int i = 0; i < 3; ++i) {
        app_rect(s,8+i*button,8,button-4,30,i==0 && learn_mode==TRAINING ? 0xb4c4 : 0x3329);
        app_text(s,8+i*button+(button-4-(int)strlen(buttons[i])*11)/2,12,buttons[i],0xffff);
    }
    const char *status = learn_mode==TRAINING ? "Learning" : learn_mode==EVALUATING ? "Evaluating" : "Paused";
    snprintf(text,sizeof text,"%s  Games %u  Moves %u",status,model.games,model.samples);
    app_text(s,10,42,text,ink);
    unsigned n = model.games < HISTORY_LEN ? model.games : HISTORY_LEN;
    uint32_t max = 1, total = 0, first = model.games-n;
    for (unsigned i = first; i < model.games; ++i) {
        uint32_t score = model.history[i%HISTORY_LEN];
        if (score > max) max = score;
        total += score;
    }
    int top = 90, bottom = h-(h >= 260 ? 103 : 80);
    snprintf(text,sizeof text,"Score / game   Mean %u",n ? total/n : 0);
    app_text(s,10,65,text,ink);
    if (bottom > top+12) {
        int left = 65, right = w-15;
        app_rect(s,left,top,right-left,bottom-top,0xffff);
        snprintf(text,sizeof text,"%u",max);
        if (n) app_text(s,5,top-4,text,ink);
        app_text(s,8,bottom-19,"0",ink);
        int lastx = left, lasty = bottom;
        for (unsigned i = 0; i < n; ++i) {
            int x = left + (right-left-2)*i/(HISTORY_LEN-1);
            int y = bottom-2 - (int)((long long)(bottom-top-3)*model.history[(first+i)%HISTORY_LEN]/max);
            if (i) chart_line(s,lastx,lasty,x,y,0x3329);
            else app_rect(s,x,y,3,3,0x3329);
            lastx=x; lasty=y;
        }
        if (!n) app_text(s,left+8,top+4,"Waiting for first game",ink);
        if (n && h >= 260) {
            snprintf(text,sizeof text,"Game %u",first+1);
            app_text(s,left,bottom+1,text,ink);
            snprintf(text,sizeof text,"%u",first+HISTORY_LEN);
            app_text(s,right-11*(int)strlen(text),bottom+1,text,ink);
        }
    }
    int y = h-73;
    if (save_failed) app_text(s,10,y,save_failed==2 ? "Checkpoint conflict" : "Save failed",0xa900);
    else if (learn_mode == EVALUATING) {
        snprintf(text,sizeof text,"Eval %u/%u games",evaluation.result[0].games+evaluation.result[1].games,2*EVAL_GAMES);
        app_text(s,10,y,text,ink);
    } else if (model.result[0].games) {
        snprintf(text,sizeof text,"Eval at move %u",model.evaluated_at);
        app_text(s,10,y,text,ink);
    } else app_text(s,10,y,"Eval: 16 paired seeds",ink);
    for (int i = 0; i < 2; ++i) {
        const EvalScore *r = learn_mode == EVALUATING ? &evaluation.result[i] : &model.result[i];
        if (r->games) {
            if (w >= 490)
                snprintf(text,sizeof text,"%s  mean %u   top %u   2048 %u/%u",i?"Fresh":"Model",r->total/r->games,r->tile,r->wins,r->games);
            else snprintf(text,sizeof text,"%s avg %u top %u",i?"Fresh":"Model",r->total/r->games,r->tile);
            app_text(s,10,y+23*(i+1),text,i ? ink : 0x3329);
        }
    }
}
