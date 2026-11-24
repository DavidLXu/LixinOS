#include "runtime.h"
#include "../console.h"
#include <stdio.h>
#include <string.h>

enum { TASKS = 16, CORES = 2 };
enum Sort { BY_CPU, BY_MEMORY, BY_TIME, BY_PID };
enum Key { UP = 300, DOWN, PAGE_UP, PAGE_DOWN, HELP, FILTER, TREE, SORT, QUIT };

struct process {
    unsigned long pid, parent, ticks, bytes, state, background;
    char name[32];
};

struct sample {
    unsigned long uptime, hz, available, cores, idle[CORES];
    unsigned long wall, average_idle, ram;
    struct process process[TASKS];
};

struct monitor {
    struct sample now, previous;
    int count, previous_count, percent[TASKS], cpu[CORES];
    int order[TASKS], depth[TASKS], visible, cursor, offset;
    unsigned long selected;
    enum Sort sort, sort_before_menu;
    int tree, editing, help, sorting;
    char filter[32];
};

static const char *const sorts[] = {"CPU%", "MEM%", "TIME+", "PID"};
static int percent(unsigned long part, unsigned long total) {
    if (!total) return -1;
    return part >= total ? 100 : (int)(part * 100 / total);
}

static int sample(struct monitor *m) {
    long bytes = console_call(16, 1);
    size_t header = sizeof(struct sample) - TASKS * sizeof(struct process);
    if (bytes < (long)header || bytes > (long)sizeof(m->now) ||
        (bytes - header) % sizeof(struct process)) return 0;
    m->previous = m->now;
    m->previous_count = m->count;
    memcpy(&m->now, (void *)0x443d8000, bytes);
    if (!m->now.hz || !m->now.cores || m->now.cores > CORES) return 0;
    m->count = (bytes - header) / sizeof(struct process);
    unsigned long elapsed = m->now.wall - m->previous.wall;
    for (int i = 0; i < (int)m->now.cores; i++) {
        unsigned long idle = m->now.idle[i] - m->previous.idle[i];
        m->cpu[i] = m->previous.wall ? percent(elapsed > idle ? elapsed - idle : 0, elapsed) : -1;
    }
    for (int i = 0; i < m->count; i++) {
        struct process *p = &m->now.process[i];
        p->name[31] = 0;
        for (char *c = p->name; *c; c++) if ((unsigned char)*c < 32 || *c == 127) *c = '?';
        m->percent[i] = -1;
        for (int j = 0; j < m->previous_count; j++) {
            struct process *old = &m->previous.process[j];
            if (p->pid == old->pid && p->ticks >= old->ticks)
                m->percent[i] = percent(p->ticks - old->ticks, elapsed);
        }
    }
    return 1;
}

static unsigned long rank(const struct monitor *m, int i) {
    const struct process *p = &m->now.process[i];
    switch (m->sort) {
    case BY_CPU: return m->percent[i] + 1;
    case BY_MEMORY: return p->bytes;
    case BY_TIME: return p->ticks;
    default: return 0;
    }
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int matches(const char *name, const char *pattern) {
    if (!*pattern) return 1;
    for (; *name; name++) {
        int i = 0;
        while (pattern[i] && name[i] && lower(name[i]) == lower(pattern[i])) i++;
        if (!pattern[i]) return 1;
    }
    return 0;
}

static void append(struct monitor *m, int i, int depth) {
    if (matches(m->now.process[i].name, m->filter)) {
        m->order[m->visible++] = i;
        m->depth[i] = depth;
    }
}

static void branch(struct monitor *m, int i, const int *sorted, int depth, unsigned *visited) {
    if (*visited & (1u << i)) return;
    *visited |= 1u << i;
    append(m, i, depth);
    for (int j = 0; j < m->count; j++) {
        int child = sorted[j];
        if (m->now.process[child].parent == m->now.process[i].pid)
            branch(m, child, sorted, depth + 1, visited);
    }
}

static void arrange(struct monitor *m) {
    int sorted[TASKS];
    for (int i = 0; i < m->count; i++) {
        int j = i;
        while (j > 0) {
            int other = sorted[j-1];
            if (rank(m, other) > rank(m, i) ||
                (rank(m, other) == rank(m, i) && m->now.process[other].pid < m->now.process[i].pid)) break;
            sorted[j] = other;
            j--;
        }
        sorted[j] = i;
    }
    m->visible = 0;
    unsigned visited = 0;
    for (int j = 0; j < m->count; j++) {
        int i = sorted[j];
        if (!m->tree) { append(m, i, 0); continue; }
        int k = 0;
        while (k < m->count && m->now.process[k].pid != m->now.process[i].parent) k++;
        if (k == m->count) branch(m, i, sorted, 0, &visited);
    }
    if (m->tree)
        for (int j = 0; j < m->count; j++) branch(m, sorted[j], sorted, 0, &visited);
    if (m->cursor >= m->visible) m->cursor = m->visible ? m->visible - 1 : 0;
    for (int j = 0; j < m->visible; j++)
        if (m->now.process[m->order[j]].pid == m->selected) m->cursor = j;
    m->selected = m->visible ? m->now.process[m->order[m->cursor]].pid : 0;
}

static void output(const char *s) { screen_write(s, strlen(s)); }
static void line(int row, int width, const char *style, const char *text) {
    char pos[24];
    snprintf(pos, sizeof(pos), "\033[%d;1H", row);
    output(pos);
    output(style);
    size_t len = strlen(text);
    screen_write(text, len < (size_t)width ? len : (size_t)width);
    output("\033[K\033[0m");
}

static void meter(int row, int width, const char *label, int value, const char *detail, const char *color) {
    char bar[33], buf[128];
    int length = width - (int)strlen(detail) - 10;
    if (length < 1) length = 1;
    if (length > 32) length = 32;
    int fill = value < 0 ? 0 : value * length / 100;
    memset(bar, '|', fill);
    memset(bar + fill, ' ', length-fill);
    bar[length] = 0;
    snprintf(buf, sizeof(buf), "%-4s [%s] %s", label, bar, detail);
    line(row, width, color, buf);
}

static int draw(struct monitor *m, int rows, int cols) {
    char buf[192], label[16], detail[64];
    if (rows < 7 || cols < 21) {
        line(1, cols, "", "htop: enlarge terminal");
        return 1;
    }
    if (m->help) {
        const char *help[] = {"htop - interactive process viewer", "Up/Down, PgUp/PgDn   Select process",
            "F4 or /   Filter command names", "F5 or t   Toggle process tree",
            "F6        Choose sort column", "P CPU%    M memory    T CPU time",
            "PRIVATE   Process-private memory", "R running/ready   S sleeping/waiting   Z exited",
            "Enter or Esc to return"};
        for (unsigned i = 0; i < sizeof(help)/sizeof(help[0]) && (int)i < rows; i++)
            line(i+1, cols, i ? "" : "\033[36m", help[i]);
        return 1;
    }
    int row = 1;
    for (int i = 0; i < (int)m->now.cores; i++) {
        snprintf(label, sizeof(label), "CPU%d", i);
        if (m->cpu[i] < 0) strcpy(detail, "--%");
        else snprintf(detail, sizeof(detail), "%d%%", m->cpu[i]);
        meter(row++, cols, label, m->cpu[i], detail, "\033[32m");
    }
    unsigned long ram = m->now.ram, used = ram > m->now.available ? ram - m->now.available : 0;
    snprintf(detail, sizeof(detail), "%lu/%lu MiB", used >> 20, ram >> 20);
    meter(row++, cols, "Mem", percent(used, ram), detail, "\033[36m");
    int running = 0;
    for (int i = 0; i < m->count; i++) running += m->now.process[i].state == 1;
    unsigned long sec = m->now.uptime / m->now.hz;
    snprintf(buf, sizeof(buf), "Tasks: %d, %d running   Uptime: %lu:%02lu:%02lu",
             m->count, running, sec/3600, sec/60%60, sec%60);
    line(row++, cols, "", buf);
    int wide = cols >= 57;
    line(row++, cols, "\033[30;42m", wide ? "  PID  PPID S CPU% PRIVATE KiB  MEM%      TIME+ Command" : "  PID S CPU% Command");
    int page = rows - row;
    if (m->cursor < m->offset) m->offset = m->cursor;
    if (m->cursor >= m->offset + page) m->offset = m->cursor - page + 1;
    for (int j = 0; j < page; j++) {
        int index = m->offset + j;
        if (index >= m->visible) {
            line(row++, cols, "", !m->visible && !j ? "No matching processes" : "");
            continue;
        }
        int i = m->order[index];
        struct process *p = &m->now.process[i];
        char state = p->state == 5 ? 'Z' : p->state <= 2 ? 'R' : 'S';
        char cpu[8];
        if (m->percent[i] < 0) strcpy(cpu, "--");
        else snprintf(cpu, sizeof(cpu), "%d", m->percent[i]);
        unsigned long seconds = p->ticks / m->now.hz;
        const char *name = *p->name ? p->name : "[unnamed]";
        if (wide) snprintf(buf, sizeof(buf), "%5lu %5lu %c %4s %11lu %5d %3lu:%02lu.%02lu %*s%s",
                    p->pid, p->parent, state, cpu, p->bytes >> 10, percent(p->bytes, ram),
                    seconds/60, seconds%60, (p->ticks % m->now.hz) * 100 / m->now.hz, m->depth[i]*2, "", name);
        else snprintf(buf, sizeof(buf), "%5lu %c %4s %*s%s", p->pid, state, cpu, m->depth[i]*2, "", name);
        line(row++, cols, index == m->cursor ? "\033[30;46m" : "", buf);
    }
    if (m->editing || *m->filter) snprintf(buf, sizeof(buf), "Filter: %s%s", m->filter, m->editing ? "_" : "   / edit   Esc clear   q quit");
    else if (cols < 51) snprintf(buf, sizeof(buf), "? Help  / Filter  q Quit");
    else snprintf(buf, sizeof(buf), "F1 Help  F4 Filter  F5 Tree  F6 Sort [%s]  F10 Quit", sorts[m->sort]);
    if (m->sorting) snprintf(buf, sizeof(buf), "Sort by: %s   Up/Down   Enter", sorts[m->sort]);
    line(rows, cols, "\033[30;46m", buf);
    return page;
}

static int key(int c) {
    if (c != 27) return c;
    c = read_timed(70);
    if (c != '[' && c != 'O') return 27;
    int n = 0;
    while ((c = read_timed(70)) >= '0' && c <= '9') if (n < 1000) n = n * 10 + c - '0';
    if (c == 'A') return UP;
    if (c == 'B') return DOWN;
    if (c == 'P') return HELP;
    if (c == 'S') return FILTER;
    if (c == '~') {
        switch (n) {
        case 5: return PAGE_UP; case 6: return PAGE_DOWN;
        case 11: return HELP; case 14: return FILTER;
        case 15: return TREE; case 17: return SORT; case 21: return QUIT;
        }
    }
    return 0;
}

void htop(void) {
    static struct monitor m;
    memset(&m, 0, sizeof(m));
    if (!sample(&m)) { output("htop: process statistics unavailable\r\n"); return; }
    output("\033[?1049h\033[?25l\033[2J");
    for (;;) {
        arrange(&m);
        unsigned long size = screen_info();
        int page = draw(&m, (size >> 16) & 65535, size & 65535);
        int c = key(read_timed(1000));
        if (c == 256 || c < 0) {
            if (!sample(&m)) break;
            continue;
        }
        if (c == 3 || c == 4 || c == QUIT) break;
        if (m.help) {
            m.help = 0;
            output("\033[2J");
        } else if (m.editing) {
            size_t n = strlen(m.filter);
            if (c == 27) { m.filter[0] = 0; m.editing = 0; }
            else if (c == '\r' || c == '\n') m.editing = 0;
            else if ((c == 8 || c == 127) && n) m.filter[n-1] = 0;
            else if (c >= 32 && c < 127 && n+1 < sizeof(m.filter)) { m.filter[n] = c; m.filter[n+1] = 0; }
        } else if (m.sorting) {
            if (c == UP || c == DOWN) m.sort = (m.sort + (c == UP ? 3 : 1)) % 4;
            else if (c == 27 || c == '\r' || c == '\n') {
                if (c == 27) m.sort = m.sort_before_menu;
                m.sorting = 0;
            }
        } else if (c == 'q' || c == 'Q') break;
        else if (c == 27) { if (*m.filter) m.filter[0] = 0; else break; }
        else if (c == HELP || c == '?') { m.help = 1; output("\033[2J"); }
        else if (c == FILTER || c == '/') m.editing = 1;
        else if (c == TREE || c == 't') m.tree = !m.tree;
        else if (c == SORT) { m.sort_before_menu = m.sort; m.sorting = 1; }
        else if (c == 'P' || c == 'M' || c == 'T') m.sort = c == 'P' ? BY_CPU : c == 'M' ? BY_MEMORY : BY_TIME;
        else if (c == UP || c == DOWN || c == PAGE_UP || c == PAGE_DOWN) {
            int next = m.cursor + (c == UP ? -1 : c == DOWN ? 1 : c == PAGE_UP ? -page : page);
            if (next >= m.visible) next = m.visible - 1;
            if (next < 0) next = 0;
            m.cursor = next;
            m.selected = m.visible ? m.now.process[m.order[next]].pid : 0;
        }
    }
    output("\033[0m\033[?25h\033[?1049l");
}
