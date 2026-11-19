/* Private services for directly spawned agent helpers. Model policy and UI
 * live in EL0. A candidate interpreter never receives these capabilities. */
int agent_test_expired(void) {
  struct process *p = &processes[bank];
  if ((p->test.deadline || p->command_deadline) && console_ready()) {
    int k = console_key();
    if (k == 3 || k == 7) return 125;
    if (k == 16) return 126;
  }
  if (p->test.overflow)
    return 123;
  if ((p->test.deadline && clock_ticks() >= p->test.deadline) ||
      (p->command_deadline && clock_ticks() >= p->command_deadline))
    return 124;
  return 0;
}
struct agent_packet {
  const u8 *bytes;
  unsigned size;
};
static int take_field(struct agent_packet *packet, struct file *field) {
  if (packet->size < 4)
    return 0;
  unsigned n = le32(packet->bytes);
  if (n > packet->size - 4)
    return 0;
  *field = (struct file){packet->bytes + 4, n};
  packet->bytes += 4 + n;
  packet->size -= 4 + n;
  return 1;
}
static int path_field(struct agent_packet *packet, char *path) {
  struct file f;
  if (!take_field(packet, &f) || !f.size || f.size > 512)
    return 0;
  memcpy(path, f.data, f.size);
  path[f.size] = 0;
  return length(path, 513) == f.size && canonical_path(path);
}
static long tested(struct process *p, struct agent_packet packet, u8 *out,
                   unsigned *used) {
  struct file code, input, runtime;
  char path[513];
  if (!path_field(&packet, path) || !take_field(&packet, &code) ||
      !take_field(&packet, &input) || packet.size || code.size > 16384 ||
      !lookup("/lib/agent-test.bin", &runtime) || runtime.size >= MAIL - USER)
    return 1;
  u8 *image = physical();
  memset(image, 0, 2 * BLOCK);
  memcpy(image, runtime.data, runtime.size);
  unsigned offset = length(path, 513) + 1;
  memcpy(image + MAIL - USER, path, offset);
  memcpy(image + MAIL - USER + offset, code.data, code.size);
  p->test.input = input.data;
  p->test.remaining = input.size;
  p->test.output = out;
  p->test.used = 0;
  p->test.deadline = clock_ticks() + 2 * clock_hz();
  sync_image();
  long result = enter_el0(MAIL, MAIL + offset, code.size, 0);
  if (p->test.overflow)
    result = 123;
  p->test.overflow = 0;
  *used = p->test.used;
  p->test.deadline = 0;
  return result == 256 ? 1 : result;
}
/* All preconditions are checked against a private snapshot. A failed edit
 * publishes neither file data nor the revision or undo metadata. */
static int apply_agent(struct agent_packet request) {
  struct file previous, versions, live = agent_versions();
  if (!take_field(&request, &previous) || !take_field(&request, &versions) ||
      previous.size != live.size ||
      memcmp(previous.data, live.data, live.size) || !batch_begin(bank))
    return 1;
  if (!stage_versions(bank, 0, 0))
    goto fail;
  while (request.size) {
    char path[513], child[513];
    struct file before, after, current;
    if (!path_field(&request, path) || proc_path(path) ||
        !take_field(&request, &before) || !take_field(&request, &after) ||
        !before.size || !after.size)
      goto fail;
    int exists = batch_lookup(bank, path, &current);
    int tag = !exists ? 'M' : current.size == 0xffffffff ? 'D' : 'F';
    if (*before.data != tag ||
        (tag == 'F' && (current.size != before.size - 1 ||
                        memcmp(current.data, before.data + 1, current.size))))
      goto fail;
    if (tag == 'D' && *after.data != 'D' &&
        batch_child(bank, path, 0, child) != -1)
      goto fail;
    struct file value = {after.data + 1, after.size - 1};
    if (*after.data == 'D')
      value.size = 0xffffffff;
    else if (*after.data != 'F' && *after.data != 'M')
      goto fail;
    if (!batch_set(bank, path, *after.data == 'M' ? 0 : &value))
      goto fail;
  }
  if (!stage_versions(bank, versions.data, versions.size))
    goto fail;
  return batch_commit(bank) ? 0 : 1;
fail:
  batch_abort(bank);
  return 1;
}
static long agent_io(struct process *p, const char *action) {
  unsigned limit = 2u << 20, size = 0, used = 0;
  u8 *payload = file_buffer(limit), *out = file_buffer(limit);
  long status = 1;
  if (!payload || !out)
    goto done;
  while (size < limit) {
    unsigned room = limit - size;
    long n = stream_read(p, payload + size, room < 4096 ? room : 4096);
    if (n < 0)
      goto done;
    if (!n)
      break;
    size += n;
  }
  if (size >= limit)
    goto done;
  struct agent_packet request = {payload, size};
  if (equal(action, "model")) {
    long n = model_request(payload, size, out, 512 * 1024);
    if (n < 0) {
      status = n == -2 ? 125 : n == -3 ? 126 : 1;
      goto done;
    }
    used = n;
    status = 0;
  } else if (equal(action, "test"))
    status = tested(p, request, out, &used);
  else if (equal(action, "header") && size >= 1 && size <= 513 &&
           payload[0] <= 2) {
    payload[size] = 0;
    display_agent_bar(payload[0], (char *)payload + 1);
    status = 0;
  } else if (equal(action, "apply"))
    status = apply_agent(request);
  else if (equal(action, "versions")) {
    struct file f = agent_versions();
    if (f.size > limit)
      goto done;
    memcpy(out, f.data, f.size);
    used = f.size;
    status = 0;
  }
  if (used && stream_write(p, 1, out, used, 1) != (long)used)
    status = 1;
done:
  release_buffer(out, limit);
  release_buffer(payload, limit);
  return status;
}
