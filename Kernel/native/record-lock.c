/* Both kernels serialize service calls. A native context may multiplex several
 * independent Linux processes, so the owner is the pair (context, guest). */
#include "record-lock.h"
#include "native.h"

enum { FILES = 128, RANGES = 512 };
static struct {
    char path[513];
    unsigned length, refs;
} names[FILES];
struct range {
    unsigned long owner, first, last;
    unsigned guest, file, type;
};
static struct range ranges[RANGES], next[RANGES];
static unsigned count;
static unsigned long serial;

static int name_equal(unsigned file, const char *path, unsigned length) {
    if (names[file].length != length) return 0;
    for (unsigned i = 0; i < length; i++)
        if (names[file].path[i] != path[i]) return 0;
    return 1;
}
static int append(unsigned *used, struct range r) {
    if (*used == RANGES) return 0;
    next[(*used)++] = r;
    return 1;
}
void record_release(unsigned long owner) {
    if (!owner) return;
    for (unsigned i = 0; i < count;) {
        if (ranges[i].owner != owner) { i++; continue; }
        names[ranges[i].file].refs--;
        ranges[i] = ranges[--count];
    }
}
long record_call(unsigned long *owner, const char *path, unsigned length, int writable,
                 unsigned query, struct lx_record_request *req) {
    if (!length || length > 512 || req->first > req->last || req->last > LX_LOCK_LAST ||
        query > 1 || req->type > LX_LOCK_UNLOCK || (query && req->type == LX_LOCK_UNLOCK))
        return -22;
    if (!query && req->type == LX_LOCK_WRITE && !writable) return -9;
    unsigned file = FILES, empty = FILES;
    for (unsigned i = 0; i < FILES; i++) {
        if (!names[i].refs) { empty = i; continue; }
        if (name_equal(i, path, length)) { file = i; break; }
    }
    for (unsigned i = 0; i < count; i++) {
        struct range r = ranges[i];
        if (r.file != file || (r.owner == *owner && r.guest == req->guest) ||
            req->type == LX_LOCK_UNLOCK || (r.type == LX_LOCK_READ && req->type == LX_LOCK_READ) ||
            r.last < req->first || req->last < r.first) continue;
        if (!query) return -11;
        req->first = r.first;
        req->last = r.last;
        req->type = r.type;
        req->guest = r.guest;
        return 0;
    }
    if (query) { req->type = LX_LOCK_UNLOCK; return 0; }
    if (file == FILES) {
        if (req->type == LX_LOCK_UNLOCK) return 0;
        if (empty == FILES) return -37;
        file = empty;
    }
    if (!*owner) *owner = ++serial;
    struct range input = {*owner, req->first, req->last, req->guest, file, req->type};
    /* Existing ranges belonging to one owner are disjoint and coalesced. */
    for (unsigned i = 0; i < count; i++) {
        struct range r = ranges[i];
        if (r.owner == *owner && r.guest == input.guest && r.file == file &&
            r.type == input.type && r.last + 1 >= input.first && input.last + 1 >= r.first) {
            if (r.first < input.first) input.first = r.first;
            if (r.last > input.last) input.last = r.last;
        }
    }
    unsigned used = 0, refs = 0;
    for (unsigned i = 0; i < count; i++) {
        struct range r = ranges[i];
        if (r.owner != *owner || r.guest != input.guest || r.file != file ||
            r.last < input.first || input.last < r.first) {
            if (!append(&used, r)) return -37;
            refs += r.file == file;
            continue;
        }
        if (r.first < input.first) {
            struct range left = r;
            left.last = input.first - 1;
            if (!append(&used, left)) return -37;
            refs++;
        }
        if (r.last > input.last) {
            r.first = input.last + 1;
            if (!append(&used, r)) return -37;
            refs++;
        }
    }
    if (input.type != LX_LOCK_UNLOCK) {
        if (!append(&used, input)) return -37;
        refs++;
    }
    if (!names[file].refs) {
        memcpy(names[file].path, path, length);
        names[file].length = length;
    }
    names[file].refs = refs;
    memcpy(ranges, next, used * sizeof *ranges);
    count = used;
    return 0;
}
