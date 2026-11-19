#ifndef LIXIN_NATIVE_H
#define LIXIN_NATIVE_H
#define NATIVE_RAM_BYTES (1536UL << 20)
#define SNAPSHOT_SLOT_BYTES (192u << 20)
#define NATIVE_FILE_LIMIT (100u << 20)
#define NATIVE_FILE_POOL (192u << 20)
#define EDITOR_FILE_LIMIT (1u << 20)
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long u64;
struct file {
    const u8 *data;
    u32 size, birth, modified;
};
void clipboard_init(void), clipboard_poll(void), clipboard_request(int multiline),
    selection_copy(void);
int clipboard_key(void), editing_file(void);
void clipboard_copy(const u8 *bytes, unsigned n);
unsigned clipboard_document(u8 *out);
void resize_display(unsigned width, unsigned height);
int ssh_mode(void), uart_poll(void), mux_key(void);
long mux_next(void);
void uart_put(u8 c), mux_write(u8 c), mux_close(void), console_pump(void);
void serial_put(unsigned char c);
int console_read(unsigned timeout_ms);
u64 clock_hz(void), clock_ticks(void);
void display_init(void), display_restore(void), display_byte(u8 c), input_init(void);
int keyboard_poll(void);
long tablet_poll(unsigned width, unsigned height, unsigned bar), canvas_pointer(void);
u64 canvas_size(void), geometry(void);
int present(const u64 *args, u64 arena, u64 bytes);
void *memmove(void *out, const void *in, u64 n);
void serial_text(const char *text);
void power_off(void);
u8 *file_buffer(u32 size);
void release_buffer(const u8 *bytes, unsigned size);
int mount_disk(void), disk_full(void);
int lookup(const char *path, struct file *file);
void *memcpy(void *out, const void *in, u64 n);
void *memset(void *out, int c, u64 n);
int batch_begin(int id), batch_commit(int id);
void batch_abort(int id);
int canonical_path(const char *path);
int batch_lookup(int id, const char *path, struct file *f);
int batch_move(int id, const char *source, const char *dest);
int batch_set(int id, const char *path, const struct file *f);
long batch_child(int id, const char *path, u64 index, char *name);
u64 disk_generation(void);
u32 disk_usage(void);
int store_file(const char *path, const u8 *data, u32 size);
long directory_entry(const char *path, u64 index, char *name);
#define NATIVE_TASKS 16
void tasks_init(void), schedule(void), select_bank(unsigned id);
unsigned task_active(void);
u64 task_pid(void);
int task_slot(void);
long task_wait_mode(u64 pid, int nohang);
long task_create(unsigned slot, int detached), task_wait(u64 pid), run_task(unsigned slot);
struct pipe;
int console_ready(void), mux_ready(void);
unsigned pipe_ready(struct pipe *p, int write);
struct pipe *pipe_create(void);
void pipe_ref(struct pipe *p, int writer, int delta);
long pipe_read(struct pipe *p, u8 *dst, unsigned size);
long pipe_write(struct pipe *p, const u8 *src, unsigned size);
void read_history(void);
const char *history_get(unsigned index);
int history_append(const char *text, unsigned len);
long file_operation(int slot, const char *cwd, u64 value, char *mail);
long git_call(int slot, const char *meta, unsigned op), git_finish(int slot, long status);
void net_init(void), net_poll(void);
long net_info(char *out);
int entropy_read(u8 *out, unsigned size);
long socket_resolve(const char *name);
int socket_connect(const char *name, unsigned port);
long socket_io(int id, int write, u8 *bytes, unsigned size);
void socket_release(int id);
unsigned socket_ready(int id);
int popup_closed(void);
int popup_open(int slot, unsigned title_width, const u32 *title, int editor), popup_show(int slot),
    popup_key(int key);
void popup_hide(int slot, int keep), task_pause(void), task_resume(int slot);
int split_mode(void), display_busy(void);
unsigned focused_pane(void), process_pane(void);
void cycle_focus(void), display_select(unsigned pane);
void display_status(void), display_idle_pointer(void), idle_slice(void);
unsigned cpu_percent(void);
u64 idle_ticks(void), heap_bytes(void);
int handles_pointer(void);
int editor_input(void), console_key(void);
unsigned long strlen(const char *s);
int memcmp(const void *a, const void *b, u64 n);
char *add_text(char *out, const char *s), *add_number(char *out, u64 value);
void file_stats(u64 *stats);
long task_list(char *out);
u64 timer_count(void);
void proc_init(void);
int proc_path(const char *path), vfs_lookup(const char *path, struct file *f);
long vfs_entry(const char *path, u64 index, char *name);
int agent_path_allowed(const char *path, int writing);
int display_application_cursor(void);
void display_agent_bar(unsigned mode, const char *root);
long model_request(const u8 *fields, unsigned n, u8 *out, unsigned cap);
struct file agent_versions(void);
int stage_versions(int slot, const u8 *bytes, unsigned size);
int run_login(void);
#endif
