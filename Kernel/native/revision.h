/* Rust stores the path-to-revision table as a JSON object after the file
 * records. Move or drop just its top-level key in the same disk transaction.
 * Nested revision/test data is copied verbatim. */
static int json_space(u8 c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}
static unsigned hex_digit(u8 c) {
  return c >= '0' && c <= '9'                 ? c - '0'
         : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10
                                              : 16;
}
static int json_key(const u8 *s, unsigned n, unsigned *at, char *out) {
  unsigned used = 0;
  if (*at >= n || s[(*at)++] != '"')
    return 0;
  while (*at < n) {
    unsigned c = s[(*at)++];
    if (c == '"') {
      out[used] = 0;
      return 1;
    }
    if (c == '\\') {
      if (*at >= n)
        return 0;
      c = s[(*at)++];
      if (c == 'u') {
        unsigned u = 0;
        for (unsigned j = 0; j < 4; j++) {
          if (*at >= n || hex_digit(s[*at]) > 15)
            return 0;
          u = 16 * u + hex_digit(s[(*at)++]);
        }
        if (u >= 0xd800 && u <= 0xdbff) {
          unsigned low = 0;
          if (n - *at < 6 || s[*at] != '\\' || s[*at + 1] != 'u')
            return 0;
          *at += 2;
          for (unsigned j = 0; j < 4; j++) {
            unsigned d = hex_digit(s[(*at)++]);
            if (d > 15)
              return 0;
            low = 16 * low + d;
          }
          if (low < 0xdc00 || low > 0xdfff)
            return 0;
          u = 0x10000 + (u - 0xd800) * 1024 + low - 0xdc00;
        } else if (u >= 0xdc00 && u <= 0xdfff)
          return 0;
        if (used > 508)
          return 0;
        if (u >= 65536)
          out[used++] = 240 | (u >> 18);
        if (u >= 2048)
          out[used++] = (u >= 65536 ? 128 : 224) | ((u >> 12) & 63);
        if (u >= 128)
          out[used++] = (u >= 2048 ? 128 : 192) | ((u >> 6) & 63);
        c = u >= 128 ? 128 | (u & 63) : u;
      } else if (c != '"' && c != '\\' && c != '/')
        return 0;
    }
    if (used >= 512 || c < 32)
      return 0;
    out[used++] = c;
  }
  return 0;
}
static int edit_revision(int slot, const char *old, const char *dest) {
  unsigned n = build_volume ? build_staged.versions.size : staged.size - staged.end;
  if (!n)
    return 1;
  const u8 *s = build_volume ? build_staged.versions.data : staged.data + staged.end;
  unsigned cap = n + 1030, at = 0, used = 1, count = 0;
  u8 *out = file_buffer(cap);
  if (!out)
    return 0;
  int ok = 0, changed = 0;
  out[0] = '{';
  while (at < n && json_space(s[at]))
    at++;
  if (at >= n || s[at++] != '{')
    goto done;
  while (at < n) {
    while (at < n && json_space(s[at]))
      at++;
    if (at < n && s[at] == '}') {
      ok = 1;
      break;
    }
    unsigned key = at;
    char path[513];
    if (!json_key(s, n, &at, path))
      goto done;
    unsigned key_end = at;
    while (at < n && json_space(s[at]))
      at++;
    if (at >= n || s[at++] != ':')
      goto done;
    while (at < n && json_space(s[at]))
      at++;
    if (at >= n || s[at] != '{')
      goto done;
    unsigned depth = 0;
    int quoted = 0;
    do {
      if (at >= n)
        goto done;
      u8 c = s[at++];
      if (quoted && c == '\\') {
        if (at >= n)
          goto done;
        at++;
        continue;
      }
      if (c == '"')
        quoted = !quoted;
      else if (!quoted && (c == '{' || c == '['))
        depth++;
      else if (!quoted && (c == '}' || c == ']'))
        depth--;
    } while (depth);
    int match = strlen(path) == strlen(old) && !memcmp(path, old, strlen(old));
    int drop =
        (match && !dest) || (!match && dest && strlen(path) == strlen(dest) &&
                             !memcmp(path, dest, strlen(dest)));
    if (match || drop)
      changed = 1;
    if (!drop) {
      if (count++)
        out[used++] = ',';
      if (match) {
        out[used++] = '"';
        for (const u8 *p = (const u8 *)dest; *p; p++) {
          if (*p < 32)
            goto done;
          if (*p == '"' || *p == '\\')
            out[used++] = '\\';
          out[used++] = *p;
        }
        out[used++] = '"';
        key = key_end;
      }
      memcpy(out + used, s + key, at - key);
      used += at - key;
    }
    while (at < n && json_space(s[at]))
      at++;
    if (at < n && s[at] == '}') {
      ok = 1;
      break;
    }
    if (at >= n || s[at++] != ',')
      goto done;
  }
  if (ok && changed) {
    out[used++] = '}';
    ok = stage_versions(slot, out, count ? used : 0);
  }
done:
  release_buffer(out, cap);
  return ok;
}
