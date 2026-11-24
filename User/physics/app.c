#include <stdio.h>
#include "../desktop/window-widgets.h"
#include "../console.h"
#include "order-swing.h"
#include "swing-weights.h"
#include "swing-presets.h"
#include "mujoco-swing.h"
#include "expression.h"

enum { RUN=1, STEP, RESET, GRAVITY, CLEAR, SEGMENT, ARC, CURVE, HINGE, SLIDER };
enum { EXAMPLES=11, EX_SINGLE=64, EX_TRIPLE, EX_SWING, EX_ORDER3, EX_ORDER10=EX_ORDER3+7, EX_MUJOCO, EX_CANCEL,
       POLICIES, POLICY_BACK, POLICY_FIRST=100 };
enum { SKY=0x1127, GRID=0x21aa, AXIS=0x3410, WHITE=0xf7df, MINT=0x76f4,
       AMBER=0xf5c9, RAIL=0x1949, SELECT=0x2450, GROUND=0xb6be };
static const unsigned short palette[]={0x5eb7,0xf5c9,0xa51f,0xf410,0x5d1e,0x8eaf};
static const char *names[]={"","运行","单步","复位","重力","清空","线段","圆弧","解析曲线","转动关节","移动关节"};
static Mechanism scene;
static int tool=SEGMENT,phase,base_id,moving_id,running,confirm_clear;
static Vec picked[3], mouse_pos, camera;
static double zoom=70,accumulator;
static long last_tick;
static const SwingPreset *policy;
static SwingMotors motors;
static MujocoSwing mujoco;
static double first_balance_time;
static int upright_steps, selected_order;
#define PRESET_COUNT ((int)(sizeof swing_presets/sizeof swing_presets[0]))
static int down,press_id,mouse_inside;
static Vec press_pos;
static const char *message;

/* Formula and gravity dialogs reuse the Desktop field editing and clipboard
 * implementation. Both place and validate the geometry before mutating it. */
static int dialog, field_index;
static char fields[4][96];
static UiText edits[4];

enum { HEADER=76, FOOT=48 };
static AppControl button(const AppWindow *s,int id) {
    int i=(id-1)%5, w=(s->width-12)/5;
    const char *name=id==RUN && running ? "暂停" : id==CLEAR && confirm_clear ? "确认清空" : names[id];
    return (AppControl){id,6+i*w,4+36*((id-1)/5),w-3,32,name};
}
static AppControl example_control(const AppWindow *s,int id) {
    if (id==EXAMPLES) return (AppControl){id,s->width-62,s->height-26,55,24,"示例"};
    static const char *options[]={"单摆","三节倒立摆","强化学习起摆","3 节策略","4 节策略","5 节策略",
                                  "6 节策略","7 节策略","8 节策略","9 节策略","10 节策略","MuJoCo 三节起摆","取消"};
    int index=id-EX_SINGLE, w=(s->width-24)/2;
    int row=(s->height-42)/7;
    if (row>44) row=44;
    return (AppControl){id,12+w*(index%2),38+row*(index/2),w-4,row-3,options[index]};
}
static int policy_controls(AppWindow *s,int x,int y,int paint) {
    int count=0;
    for (int i=0;i<PRESET_COUNT;++i) count+=swing_presets[i].order==selected_order;
    int row=(s->height-40)/(count+1), index=0;
    if (row>43) row=43;
    for (int i=0;i<=PRESET_COUNT;++i) {
        const SwingPreset *preset=i<PRESET_COUNT?&swing_presets[i]:NULL;
        if (preset && preset->order!=selected_order) continue;
        char title[100];
        if (!preset) strcpy(title,"返回示例");
        else if (!preset->seed_number) strcpy(title,"原版三节策略   测试 32/32");
        else snprintf(title,sizeof title,"种子 %d   %d 轮   测试 %d/32",
                      preset->seed_number,preset->generations,preset->test_successes);
        AppControl b={preset?POLICY_FIRST+i:POLICY_BACK,12,36+index++*row,s->width-24,row-2,title};
        if (paint) {
            app_rect(s,b.x,b.y,b.w,b.h,RAIL);
            label(s,b.x+8,b.y+(b.h>25?4:0),b.w-12,title,WHITE);
        } else if (app_control_hit(b,x,y)) return b.id;
    }
    return 0;
}
static void set_example(const AppWindow *s,int id) {
    if (id==EX_MUJOCO && !open_mujoco_swing(&mujoco)) {
        message="MuJoCo 模型加载失败"; dialog=running=0; return;
    }
    if (id!=EX_MUJOCO) close_mujoco_swing(&mujoco);
    policy=(id==EX_SWING || id==EX_MUJOCO)?&swing_presets[0]:id>=POLICY_FIRST?&swing_presets[id-POLICY_FIRST]:NULL;
    first_balance_time=0;
    upright_steps=0;
    if (policy) {
        const double down[MAX_ORDER]={0};
        setup_swing(&scene,&motors,policy->order,down);
    } else pendulum_example(&scene,id==EX_SINGLE?SINGLE_PENDULUM:TRIPLE_INVERTED);
    phase=running=dialog=confirm_clear=0;
    accumulator=0;
    camera=vec(0,0);
    double span=policy?1.30*policy->order+.35:4.25;
    zoom=fmin(70,fmin(s->width-12,s->height-HEADER-FOOT)/span);
    tool=SEGMENT;
    message=policy ? "点运行，从下垂起摆" : id==EX_TRIPLE ? "三节倒立摆：底部固定铰链，点运行释放" : "单摆：点运行释放";
}
static AppControl field_box(const AppWindow *s,int i) {
    return (AppControl){20+i,84,41+i*31,s->width-98,27,""};
}
static AppControl dialog_button(const AppWindow *s,int i) {
    return (AppControl){30+i,s->width-164+i*78,s->height-33,72,27,i?"取消":"应用"};
}
static int canvas(const AppWindow *s,int x,int y) {
    return x>=0 && x<s->width && y>=HEADER && y<s->height-FOOT;
}
static Vec screen(const AppWindow *s,Vec p) {
    return vec(s->width*0.5+(p.x-camera.x)*zoom,
               (HEADER+s->height-FOOT)*0.5-(p.y-camera.y)*zoom);
}
static Vec world(const AppWindow *s,int x,int y) {
    return vec(camera.x+(x-s->width*0.5)/zoom,
               camera.y+((HEADER+s->height-FOOT)*0.5-y)/zoom);
}
static void dot_screen(AppWindow *s,int x,int y,int r,unsigned short color) {
    for (int j=-r;j<=r;++j) for (int i=-r;i<=r;++i)
        if (i*i+j*j<=r*r && canvas(s,x+i,y+j)) s->pixels[(y+j)*s->width+x+i]=color;
}
static int clip_range(double p,double d,double lo,double hi,double *first,double *last) {
    if (fabs(d)<1e-12) return p>=lo && p<=hi;
    double a=(lo-p)/d,b=(hi-p)/d;
    if (a>b) { double t=a; a=b; b=t; }
    if (a>*first) *first=a;
    if (b<*last) *last=b;
    return *first<=*last;
}
static void stroke(AppWindow *s,Vec a,Vec b,int r,unsigned short color) {
    Vec p=screen(s,a), d=sub(screen(s,b),p);
    double first=0,last=1;
    if (!clip_range(p.x,d.x,0,s->width-1,&first,&last) ||
        !clip_range(p.y,d.y,HEADER,s->height-FOOT-1,&first,&last)) return;
    Vec start=add(p,mul(d,first)), delta=mul(d,last-first);
    int steps=(int)ceil(fmax(fabs(delta.x),fabs(delta.y)));
    for (int i=0;i<=steps;++i) {
        Vec q=add(start,mul(delta,steps ? (double)i/steps : 0));
        dot_screen(s,(int)lround(q.x),(int)lround(q.y),r,color);
    }
}
static void marker(AppWindow *s,Vec p,int r,unsigned short color) {
    Vec q=screen(s,p);
    if (q.x < -r || q.x > s->width+r || q.y < HEADER-r || q.y > s->height-FOOT+r) return;
    dot_screen(s,(int)lround(q.x),(int)lround(q.y),r,color);
}
static int hit_link(Vec p,int exclude) {
    int hit=WORLD;
    double closest=(11/zoom)*(11/zoom);
    for (int k=scene.link_count-1;k>=0;--k) if (k!=exclude) {
        Link *l=&scene.links[k]; Vec local=local_point(l,p);
        for (int i=1;i<l->count;++i) {
            Vec a=l->point[i-1],d=sub(l->point[i],a),v=sub(local,a);
            double d2=dot(d,d), t=d2 ? fmin(1,fmax(0,dot(v,d)/d2)) : 0;
            Vec gap=sub(v,mul(d,t));
            if (dot(gap,gap)<closest) { hit=k; closest=dot(gap,gap); }
        }
    }
    return hit;
}
static void rewind_app(void) {
    running=phase=0; accumulator=0; rewind_scene(&scene);
    if (mujoco.data) mj_resetData(mujoco.model,mujoco.data);
    first_balance_time=0;
    upright_steps=0; memset(motors.torque,0,sizeof motors.torque);
}
static void begin_dialog(int kind) {
    dialog=kind; field_index=0; message=NULL;
    if (kind==CURVE) {
        strcpy(fields[0],"t"); strcpy(fields[1],"0.5*sin(pi*t)");
        strcpy(fields[2],"0"); strcpy(fields[3],"2");
    } else {
        snprintf(fields[0],sizeof fields[0],"%.6g",scene.gravity.x);
        snprintf(fields[1],sizeof fields[1],"%.6g",scene.gravity.y);
    }
    for (int i=0;i<(kind==CURVE?4:2);++i) ui_text_begin(&edits[i],fields[i],1);
}
static void finish_dialog(void) {
    double a,b;
    if (dialog==GRAVITY) {
        if (!formula_value(fields[0],0,&a) || !formula_value(fields[1],0,&b)) {
            message="请输入有效的重力分量"; return;
        }
        scene.gravity=vec(a,b);
    } else {
        if (!formula_value(fields[2],0,&a) || !formula_value(fields[3],0,&b) || a==b) {
            message="参数区间无效"; return;
        }
        Vec points[CURVE_POINTS];
        for (int i=0;i<CURVE_POINTS;++i) {
            double t=a+(b-a)*i/(CURVE_POINTS-1); Vec q;
            if (!formula_value(fields[0],t,&q.x) || !formula_value(fields[1],t,&q.y)) {
                message="表达式或定义域无效"; return;
            }
            points[i]=add(picked[0],q);
        }
        if (add_link(&scene,points,CURVE_POINTS)<0) { message="曲线长度无效或连杆已满"; return; }
        policy=NULL;
    }
    message=NULL; phase=dialog=0;
}
static void advance_scene(void) {
    if (!policy) { physics_step(&scene,1.0/120); return; }
    if (mujoco.data) advance_mujoco_swing(&mujoco,&scene,&motors,policy->weights);
    else advance_swing(&scene,&motors,policy->weights);
    if (!swing_is_balanced(&scene,&motors)) upright_steps=0;
    else if (upright_steps<240) {
        ++upright_steps;
        if (upright_steps==240 && first_balance_time==0) first_balance_time=scene.time;
    }
}
static void tool_action(int id) {
    message=NULL;
    if (id!=CLEAR) confirm_clear=0;
    if (id==RUN) {
        running=!running; phase=0; accumulator=0; last_tick=console_call(38,0);
    } else if (id==STEP) {
        running=phase=0; advance_scene();
    } else if (id==RESET) rewind_app();
    else if (id==CLEAR) {
        if (!confirm_clear) { running=0; confirm_clear=1; message="再次点击清空，删除全部连杆和关节"; }
        else { rewind_app(); scene.link_count=scene.joint_count=confirm_clear=0; policy=NULL; }
    } else if (id==EXAMPLES) { running=phase=0; dialog=EXAMPLES; }
    else if (id==GRAVITY) { running=phase=0; begin_dialog(GRAVITY); }
    else { rewind_app(); tool=id; }
}
static void place(Vec p) {
    message=NULL; confirm_clear=0;
    if (running) return;
    if (scene.time!=0) rewind_app();
    if (tool==CURVE) { picked[0]=p; begin_dialog(CURVE); return; }
    if (tool==SEGMENT || tool==ARC) {
        picked[phase++]=p;
        int n=tool==SEGMENT ? 2 : 3;
        if (phase<n) return;
        Vec arc[CURVE_POINTS];
        int id=tool==SEGMENT ? add_link(&scene,picked,2) :
            add_link(&scene,arc,arc_points(picked[0],picked[1],picked[2],arc));
        phase=0;
        if (id<0) message="几何无效或连杆已满，请重新取点";
        else policy=NULL;
        return;
    }
    if (!phase) { base_id=hit_link(p,WORLD); phase=1; return; }
    if (phase==1) {
        moving_id=hit_link(p,base_id);
        if (moving_id==WORLD) { message="请在另一根连杆上选锚点"; return; }
        picked[0]=p;
        if (tool==SLIDER) { phase=2; return; }
    }
    Vec axis=sub(p,picked[0]);
    if (!add_joint(&scene,tool==HINGE?REVOLUTE:PRISMATIC,base_id,moving_id,picked[0],axis)) {
        message="关节已满或轴线太短"; return;
    }
    phase=0; policy=NULL;
}
static const char *hint(void) {
    if (message) return message;
    if (running) return "仿真运行中";
    if (tool==CURVE) return "点击曲线的局部原点，再输入公式";
    if (tool==SEGMENT) return phase ? "点击线段终点" : "点击两端或拖动创建线段";
    if (tool==ARC) return phase==0?"点击圆弧起点":phase==1?"点击弧上的中间点":"点击圆弧终点";
    if (!phase) return "选择基座连杆，点空白处表示固定到地面";
    if (phase==2) return "点击第二点指定移动轴方向";
    return base_id==WORLD ? "地面已选，点击连杆上的关节锚点" : "基座已选，点击另一根连杆上的锚点";
}
static void draw_dialog(AppWindow *s) {
    app_rect(s,0,0,s->width,s->height,0xe71c);
    if (dialog==POLICIES) {
        char title[80];
        snprintf(title,sizeof title,"%d 节起摆   独立测试通过次数 / 32",selected_order);
        label(s,14,6,s->width-26,title,RAIL);
        policy_controls(s,0,0,1);
        return;
    }
    if (dialog==EXAMPLES) {
        label(s,14,12,s->width-26,"选择示例将替换当前场景",RAIL);
        for (int i=EX_SINGLE;i<=EX_CANCEL;++i) {
            AppControl b=example_control(s,i);
            app_rect(s,b.x,b.y,b.w,b.h,i==EX_TRIPLE?SELECT:RAIL);
            label(s,b.x+12,b.y+5,b.w-18,b.name,WHITE);
        }
        return;
    }
    label(s,14,8,s->width-22,dialog==CURVE?"参数曲线  局部坐标 (m)":"重力加速度 (m/s^2)",RAIL);
    const char *labels[]={"x(t)","y(t)","t min","t max"};
    int n=dialog==CURVE?4:2;
    for (int i=0;i<n;++i) {
        AppControl b=field_box(s,i);
        label(s,12,b.y+2,67,dialog==CURVE?labels[i]:(i?"gy":"gx"),RAIL);
        app_field(s,b,&edits[i],fields[i],field_index==i);
    }
    if (message) label(s,10,s->height-31,s->width-178,message,0xa900);
    else if (s->height>240)
        label(s,12,46+31*n,s->width-22,dialog==CURVE?"t, pi, + - * / ^, sin cos tan sqrt exp log abs":"y 轴向上，向下的重力取负值",RAIL);
    for (int i=0;i<2;++i) { AppControl b=dialog_button(s,i);
        app_rect(s,b.x,b.y,b.w,b.h,i?GRID:SELECT);
        label(s,b.x+17,b.y+2,b.w-20,b.name,WHITE);
    }
}
static void draw(AppWindow *s) {
    if (dialog) { draw_dialog(s); return; }
    app_rect(s,0,0,s->width,s->height,SKY);
    Vec lo=world(s,0,s->height-FOOT),hi=world(s,s->width,HEADER);
    for (int x=(int)ceil(lo.x);x<=hi.x;++x) stroke(s,vec(x,lo.y),vec(x,hi.y),0,x?GRID:AXIS);
    for (int y=(int)ceil(lo.y);y<=hi.y;++y) stroke(s,vec(lo.x,y),vec(hi.x,y),0,y?GRID:AXIS);
    for (int k=0;k<scene.link_count;++k) {
        Link *l=&scene.links[k];
        unsigned short col=phase && tool>=HINGE && k==base_id ? WHITE : palette[k%6];
        double c=cos(l->angle),sn=sin(l->angle);
        Vec prev=world_point(l,l->point[0]);
        marker(s,prev,4,col);
        for (int i=1;i<l->count;++i) {
            Vec p=l->point[i],next=add(l->center,vec(c*p.x-sn*p.y,sn*p.x+c*p.y));
            stroke(s,prev,next,2,col); prev=next;
        }
        marker(s,prev,4,col);
    }
    for (int i=0;i<scene.joint_count;++i) {
        Joint *j=&scene.joints[i]; Link *l=j->a==WORLD?NULL:&scene.links[j->a];
        Vec p=world_point(&scene.links[j->b],j->anchor_b),base=l?world_point(l,j->anchor_a):j->anchor_a;
        if (j->type==PRISMATIC) {
            Vec axis=rotate(j->axis_a,l?l->angle:0);
            stroke(s,sub(base,mul(axis,1.1)),add(base,mul(axis,1.1)),1,GROUND);
            stroke(s,sub(p,mul(perp(axis),0.10)),add(p,mul(perp(axis),0.10)),2,AMBER);
        } else {
            int radius=policy?(int)fmax(2,fmin(7,zoom*.17)):7;
            marker(s,p,radius,AMBER); marker(s,p,(radius-1)/2,SKY);
        }
        if (!l) {
            Vec left=add(base,vec(-.18,-.22)),right=add(base,vec(.18,-.22));
            stroke(s,base,left,1,GROUND); stroke(s,base,right,1,GROUND);
            stroke(s,sub(left,vec(.06,0)),add(right,vec(.06,0)),1,GROUND);
            for (int k=-1;k<=1;++k) {
                Vec foot=add(base,vec(.13*k,-.22));
                stroke(s,foot,sub(foot,vec(.08,.08)),1,GROUND);
            }
        }
    }
    if (!running && mouse_inside && (tool==SEGMENT || tool==ARC) && (phase || down)) {
        Vec from=phase?picked[0]:press_pos;
        if (tool==ARC && phase==2) {
            Vec arc[CURVE_POINTS]; int n=arc_points(from,picked[1],mouse_pos,arc);
            for (int i=1;i<n;++i) stroke(s,arc[i-1],arc[i],1,GROUND);
        } else stroke(s,from,mouse_pos,1,GROUND);
        marker(s,from,4,WHITE);
    }
    if (tool==SLIDER && phase==2 && mouse_inside) stroke(s,picked[0],mouse_pos,1,AMBER);
    for (int id=RUN;id<=SLIDER;++id) {
        AppControl b=button(s,id);
        int active=id==tool || (id==RUN && running) || (id==CLEAR && confirm_clear);
        app_rect(s,b.x,b.y,b.w,b.h,active?SELECT:RAIL);
        int width=(int)strlen(b.name)/3*16;
        label(s,b.x+(b.w>width?(b.w-width)/2:1),b.y+5,b.w-2,b.name,active?MINT:WHITE);
    }
    char buf[110];
    if (mujoco.data && policy)
        snprintf(buf,sizeof buf,"MuJoCo %s   3 links   %.2f s",mj_versionString(),scene.time);
    else if (policy && policy->seed_number)
        snprintf(buf,sizeof buf,"%d 节  种子 %d  %d 轮  测试 %d/32  %.2f s",
                 policy->order,policy->seed_number,policy->generations,policy->test_successes,scene.time);
    else snprintf(buf,sizeof buf,"%d links   %d joints   %.2f s   g=(%.2g, %.2g) m/s^2",
             scene.link_count,scene.joint_count,scene.time,scene.gravity.x,scene.gravity.y);
    if (policy && first_balance_time>0) {
        size_t used=strlen(buf);
        snprintf(buf+used,sizeof buf-used,"  首次稳住 %.2f s",first_balance_time);
    }
    app_rect(s,0,s->height-FOOT,s->width,FOOT,RAIL);
    label(s,10,s->height-FOOT+1,s->width-18,buf,MINT);
    AppControl examples=example_control(s,EXAMPLES);
    const char *status=hint();
    if (policy && !message && !phase && (running || scene.time>0)) {
        const char *state=upright_steps>=240?"已稳住":upright_steps?"平衡中":"起摆中";
        if (policy->order==3)
            snprintf(buf,sizeof buf,"%s  %s  %.1f / %.1f / %.1f Nm",
                     running?"策略":"暂停",state,motors.torque[0],motors.torque[1],motors.torque[2]);
        else snprintf(buf,sizeof buf,"%d 节  %s  %s  底座 %.1f Nm",
                 policy->order,running?"运行":"暂停",state,motors.torque[0]);
        status=buf;
    }
    label(s,10,s->height-24,examples.x-15,status,WHITE);
    app_rect(s,examples.x,examples.y,examples.w,examples.h,SELECT);
    label(s,examples.x+10,examples.y,examples.w-12,examples.name,MINT);
}
static int control_hit(AppWindow *s,int x,int y) {
    if (dialog==POLICIES) return policy_controls(s,x,y,0);
    if (dialog==EXAMPLES) {
        for (int i=EX_SINGLE;i<=EX_CANCEL;++i)
            if (app_control_hit(example_control(s,i),x,y)) return i;
        return 0;
    }
    if (dialog) {
        int n=dialog==CURVE?4:2;
        for (int i=0;i<n;++i) if (app_control_hit(field_box(s,i),x,y)) return 20+i;
        for (int i=0;i<2;++i) if (app_control_hit(dialog_button(s,i),x,y)) return 30+i;
        return 0;
    }
    for (int i=RUN;i<=SLIDER;++i) if (app_control_hit(button(s,i),x,y)) return i;
    if (app_control_hit(example_control(s,EXAMPLES),x,y)) return EXAMPLES;
    return canvas(s,x,y)?40:0;
}
static int event(AppWindow *s,WindowEvent e) {
    int changed=0;
    if (e.type==WINDOW_CLIENT_CONFIGURE && !last_tick && scene.time==0) {
        zoom=fmin(70,(s->height-HEADER-FOOT)/4.25);
        last_tick=console_call(38,0);
    }
    if (e.type==WINDOW_CLIENT_TIMER && running) {
        long now=console_call(38,0);
        accumulator+=fmin(0.05,fmax(0,(now-last_tick)/1000.0)); last_tick=now;
        while (accumulator>=1.0/120) { advance_scene(); accumulator-=1.0/120; }
        changed=1;
    } else if (e.type==WINDOW_CLIENT_KEY) {
        if (dialog==EXAMPLES || dialog==POLICIES) {
            if (e.value==27) dialog=dialog==POLICIES?EXAMPLES:0;
        } else if (dialog) {
            if (e.value==27) { dialog=phase=0; message=NULL; }
            else if (e.value==9) { field_index=(field_index+1)%(dialog==CURVE?4:2); ui_text_begin(&edits[field_index],fields[field_index],1); }
            else if (e.value==13 || e.value==10) finish_dialog();
            else app_field_key(s,&edits[field_index],fields[field_index],sizeof fields[0],e.value);
        } else if (e.value==' ') tool_action(RUN);
        else if (e.value==27) { phase=confirm_clear=0; message=NULL; }
        else if (e.value=='r') tool_action(RESET);
        else if (e.value=='q') return -1;
        changed=1;
    } else if (e.type==WINDOW_CLIENT_PASTE && dialog && dialog!=EXAMPLES && dialog!=POLICIES) {
        changed=app_field_paste(s,&edits[field_index],fields[field_index],sizeof fields[0],e.value);
    } else if (e.type==WINDOW_CLIENT_WHEEL && !dialog && canvas(s,e.x,e.y)) {
        Vec fixed=world(s,e.x,e.y);
        zoom=fmin(180,fmax(2,zoom*(e.value>0?1.2:1/1.2)));
        camera=add(camera,sub(fixed,world(s,e.x,e.y))); changed=1;
    } else if (e.type==WINDOW_CLIENT_POINTER) {
        int mx=e.x,my=e.y; mouse_inside=canvas(s,mx,my);
        mouse_pos=world(s,mx,my);
        int hit=control_hit(s,mx,my);
        if (e.value && !down) { press_id=hit; press_pos=mouse_pos; down=1; }
        if (!e.value && down) {
            down=0;
            if (hit && hit==press_id) {
                if (hit==40) {
                    Vec drag=sub(mouse_pos,press_pos);
                    if (tool==SEGMENT && !phase && !running && dot(drag,drag)*zoom*zoom>64) {
                        place(press_pos); place(mouse_pos);
                    } else place(press_pos);
                } else if (hit==EX_CANCEL) dialog=0;
                else if (hit>=EX_ORDER3 && hit<=EX_ORDER10) {
                    selected_order=3+hit-EX_ORDER3; dialog=POLICIES;
                } else if (hit==POLICY_BACK) dialog=EXAMPLES;
                else if (hit==EX_MUJOCO || (hit>=EX_SINGLE && hit<=EX_SWING) ||
                         (hit>=POLICY_FIRST && hit<POLICY_FIRST+PRESET_COUNT)) set_example(s,hit);
                else if (hit>=30) {
                    if (hit==30) finish_dialog(); else { dialog=phase=0; message=NULL; }
                } else if (hit>=20) {
                    field_index=hit-20; ui_text_begin(&edits[field_index],fields[field_index],1);
                } else tool_action(hit);
            }
            changed=1;
        } else if (!dialog && !running && mouse_inside && (phase || (down && press_id==40))) changed=1;
    }
    s->timer_ms=running?16:0;
    return changed;
}
int main(void) {
    pendulum_example(&scene,TRIPLE_INVERTED);
    message="三节倒立摆：底部固定铰链，点运行释放";
    int result=app_run3(event,draw);
    close_mujoco_swing(&mujoco);
    return result;
}
