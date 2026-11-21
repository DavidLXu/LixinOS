#include "window-widgets.h"

static UiControls controls={1};
static UiText edit;
static char name[129]="你好", saved[129];
static AppControl field(AppWindow *s) { return (AppControl){1,12,12,s->width-24,28,""}; }
static AppControl button(void) { return (AppControl){2,12,50,80,26,"确定"}; }
static int event(AppWindow *s,WindowEvent e) {
    int action=0;
    if (e.type==WINDOW_CLIENT_PASTE && controls.focus==1) return app_field_paste(s,&edit,name,sizeof name,e.value);
    if (e.type==WINDOW_CLIENT_POINTER) {
        int hit=app_control_hit(field(s),e.x,e.y)?1:app_control_hit(button(),e.x,e.y)?2:0;
        int press=e.value && !controls.down;
        action=ui_pointer(&controls,hit,e.value);
        if (controls.pressed==1) {
            edit.caret=app_field_position(field(s),&edit,name,e.x);
            if (press) edit.anchor=edit.caret;
        }
    } else if (e.type==WINDOW_CLIENT_KEY) {
        int ids[]={1,2};
        if (e.value==27) return -1;
        if (e.value==9) controls.focus=ui_focus_next(controls.focus,ids,2);
        else if (controls.focus==2 && (e.value==' ' || e.value==13)) action=2;
        else if (controls.focus==1) app_field_key(s,&edit,name,sizeof name,e.value);
    }
    if (action==2) strcpy(saved,name);
    return 1;
}
static void draw(AppWindow *s) {
    app_rect(s,0,0,s->width,s->height,0xf7be);
    app_field(s,field(s),&edit,name,controls.focus==1);
    app_button(s,&controls,button(),1);
    label(s,12,88,s->width-24,saved,0x1947);
}
int main(void) { ui_text_begin(&edit,name,1); return app_run3(event,draw); }
