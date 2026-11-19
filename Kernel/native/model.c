/* The host key stays outside the guest. The EL0 broker supplies a tab-delimited
 * request; this driver owns framing, request IDs and the sole virtconsole. */
#include "virtio.h"

enum { LIMIT = 512 * 1024, CHUNK = 4096 };
static struct virtio port = {(volatile u32 *)0x0a000400};
static struct queue rx, tx;
static u8 buffers[Q][512], packet[CHUNK + 80];
static int ready, busy, interruption;
static unsigned serial, cursor;

static int initialize(void) {
  if (ready)
    return ready > 0;
  ready = -1;
  if (!device(port, 3, 0) || !setup_queue(port, 0, &rx) ||
      !setup_queue(port, 1, &tx))
    return 0;
  for (unsigned i = 0; i < Q; i++)
    submit(&rx, i, buffers[i], sizeof(buffers[i]), 1);
  port.r[0x70 / 4] = 15;
  port.r[0x50 / 4] = 0;
  ready = 1;
  return 1;
}
static int available(struct queue *q, u64 deadline) {
  while (q->seen == q->used.index) {
    if (q == &rx && console_ready()) {
      int k = console_key();
      if (k == 7 || k == 3 || k == 16) {
        interruption = k == 16 ? -3 : -2;
        return 0;
      }
    }
    if (clock_ticks() >= deadline)
      return 0;
    schedule();
  }
  BARRIER();
  return 1;
}
static int receive(u8 *out, unsigned cap, const char *prefix,
                   unsigned prefix_len, u64 deadline) {
  unsigned used = 0;
  for (;;) {
    if (!available(&rx, deadline))
      return -1;
    unsigned index = rx.seen % Q, id = rx.used.ring[index].id,
             len = rx.used.ring[index].len;
    if (id >= Q || len > sizeof(buffers[0])) {
      ready = -1;
      return -1;
    }
    while (cursor < len) {
      unsigned byte = buffers[id][cursor++];
      if (byte == '\n') {
        if (used >= prefix_len && used <= cap &&
            !memcmp(prefix, out, prefix_len)) {
          memmove(out, out + prefix_len, used - prefix_len);
          return used - prefix_len;
        }
        used = 0; /* ignore an expired request's response */
      } else if (used < cap)
        out[used++] = byte;
      else
        used = cap + 1;
    }
    cursor = 0;
    rx.seen++;
    submit(&rx, id, buffers[id], sizeof(buffers[id]), 1);
    port.r[0x50 / 4] = 0;
  }
}
long model_request(const u8 *fields, unsigned n, u8 *out, unsigned cap) {
  if (busy || !n || n > LIMIT - 24 || !initialize())
    return -1;
  for (unsigned i = 0; i < n; i++)
    if (!fields[i] || fields[i] == '\n')
      return -1;
  busy = 1;
  interruption = 0;
  char prefix[24];
  char *end = add_number(prefix, ++serial);
  *end++ = '\t';
  unsigned header = end - prefix, total = header + n;
  long result = -1;
  u64 deadline = clock_ticks() + 60 * clock_hz();
  for (unsigned at = 0; at < total; at += CHUNK) {
    unsigned count = total - at;
    if (count > CHUNK)
      count = CHUNK;
    char *p = (char *)packet;
    *p++ = '@';
    memcpy(p, prefix, header);
    p += header;
    p = add_number(p, at);
    *p++ = '\t';
    p = add_number(p, total);
    *p++ = '\t';
    for (unsigned i = at; i < at + count; i++)
      *p++ = i < header ? prefix[i] : fields[i - header];
    *p++ = '\n';
    submit(&tx, 0, packet, p - (char *)packet, 0);
    port.r[0x50 / 4] = 1;
    if (!available(&tx, deadline)) {
      ready = -1;
      break;
    }
    tx.seen++;
    long length = receive(out, cap, prefix, header, deadline);
    if (at + count == total) {
      result = length;
      break;
    }
    if (length != 3 || memcmp(out, "ack", 3))
      break;
  }
  busy = 0;
  return interruption ? interruption : result;
}
