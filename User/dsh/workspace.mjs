// Foreground commands run in a networkless guest. Only project files cross the channel.
// Simple literal read-only commands execute directly in-process for sub-millisecond latency.
import fs from 'node:fs';
import net from 'node:net';
import { join, relative, resolve, sep } from 'node:path';
import { homedir } from 'node:os';
import volume from './volume.cjs';

const bytesLimit = 8 << 20;
const frameLimit = 12 << 20;
const entriesLimit = 1024;
const builtinOutputLimit = 64 << 10;

function visible(key) {
  return key.length > 0 && Buffer.byteLength(key) <= 480 && !key.includes('\0') &&
    key.split('/').every(s => s && !s.startsWith('.'));
}

function within(path, root) {
  return path === root || path.startsWith(root === sep ? sep : root + sep);
}

function tryReadonlyBuiltin(command, root, cwd, protectedDir, timeoutMs) {
  if (typeof command !== 'string' || !command.trim() || !/^[A-Za-z0-9 ._\/-]+$/.test(command))
    return null;
  const words = command.trim().split(/\s+/);
  const cmd = words[0];
  if (!['pwd', 'ls', 'cat', 'head', 'tail', 'wc'].includes(cmd)) return null;

  const resolveSafe = (arg) => {
    if (!arg || arg.startsWith('-') || arg.split('/').includes('..')) return null;
    const abs = resolve(cwd, arg);
    if (!within(abs, root) || within(abs, protectedDir)) return null;
    const rel = relative(root, abs);
    if (rel && rel.split('/').some(s => s.startsWith('.'))) return null;
    return abs;
  };
  const ok = (text) => ({
    kind: 'foreground', cwd, exitCode: 0, signal: null, timedOut: false, aborted: false,
    timeoutMs, stdout: { text, truncated: false }, stderr: { text: '', truncated: false },
  });
  const fail = (text) => ({
    kind: 'foreground', cwd, exitCode: 1, signal: null, timedOut: false, aborted: false,
    timeoutMs, stdout: { text: '', truncated: false }, stderr: { text, truncated: false },
  });

  if (cmd === 'pwd') {
    return words.length === 1 ? ok(cwd + '\n') : null;
  }
  if (cmd === 'ls') {
    let showAll = false, longFmt = false, idx = 1;
    while (idx < words.length && words[idx].startsWith('-')) {
      const f = words[idx].slice(1);
      if (!f || !/^[1al]+$/.test(f)) return null;
      if (f.includes('a')) showAll = true;
      if (f.includes('l')) longFmt = true;
      idx++;
    }
    if (words.length - idx > 1) return null;
    const target = idx < words.length ? resolveSafe(words[idx]) : cwd;
    if (!target) return null;
    try {
      const st = fs.lstatSync(target);
      if (st.isFile()) {
        const display = words[idx] ?? '.';
        return ok(longFmt ? `- ${String(st.size).padStart(8)} ${display}\n` : `${display}\n`);
      }
      if (!st.isDirectory()) return fail('ls: unsupported entry\n');
      const names = fs.readdirSync(target).sort();
      const lines = [];
      let total = 0;
      for (const name of names) {
        if (!showAll && name.startsWith('.')) continue;
        const full = join(target, name);
        if (within(full, protectedDir) || name === '.git' || name === '.dsh') continue;
        let line;
        if (longFmt) {
          const em = fs.lstatSync(full);
          line = `${em.isDirectory() ? 'd' : '-'} ${String(em.size).padStart(8)} ${name}\n`;
        } else {
          line = `${name}\n`;
        }
        total += line.length;
        if (total > builtinOutputLimit) return null;
        lines.push(line);
      }
      return ok(lines.join(''));
    } catch (e) {
      return fail(`ls: ${e.message}\n`);
    }
  }
  if (cmd === 'cat') {
    let numbered = false, idx = 1;
    while (idx < words.length && words[idx] === '-n') { numbered = true; idx++; }
    if (idx >= words.length) return null;
    const paths = [];
    for (let i = idx; i < words.length; i++) {
      const p = resolveSafe(words[i]);
      if (!p) return null;
      paths.push(p);
    }
    try {
      let out = '', lineNo = 1;
      for (const p of paths) {
        const st = fs.lstatSync(p);
        if (!st.isFile() || st.size > builtinOutputLimit) return null;
        const content = fs.readFileSync(p, 'utf8');
        if (numbered) {
          const parts = content.match(/[^\n]*\n|[^\n]+/g) ?? [];
          for (const part of parts) {
            out += `${String(lineNo++).padStart(6)}\t${part}`;
            if (out.length > builtinOutputLimit) return null;
          }
        } else {
          out += content;
          if (out.length > builtinOutputLimit) return null;
        }
      }
      return ok(out);
    } catch (e) {
      return fail(`cat: ${e.message}\n`);
    }
  }
  if (cmd === 'head' || cmd === 'tail') {
    let count = 10, idx = 1;
    if (idx < words.length && words[idx].startsWith('-')) {
      const flag = words[idx].slice(1);
      if (flag === 'n' && idx + 1 < words.length && /^\d+$/.test(words[idx + 1])) {
        count = Number(words[idx + 1]); idx += 2;
      } else if (/^n?\d+$/.test(flag)) {
        count = Number(flag.replace(/^n/, '')); idx += 1;
      } else return null;
    }
    if (idx >= words.length) return null;
    const argsList = words.slice(idx);
    const paths = argsList.map(resolveSafe);
    if (paths.some(p => !p)) return null;
    try {
      let out = '';
      for (let i = 0; i < paths.length; i++) {
        const st = fs.lstatSync(paths[i]);
        if (!st.isFile() || st.size > 4 * 1024 * 1024) return null;
        const lines = fs.readFileSync(paths[i], 'utf8').match(/[^\n]*\n|[^\n]+/g) ?? [];
        const picked = cmd === 'head' ? lines.slice(0, count) : lines.slice(Math.max(0, lines.length - count));
        if (paths.length > 1) out += `${i > 0 ? '\n' : ''}==> ${argsList[i]} <==\n`;
        out += picked.join('');
        if (out.length > builtinOutputLimit) return null;
      }
      return ok(out);
    } catch (e) {
      return fail(`${cmd}: ${e.message}\n`);
    }
  }
  if (cmd === 'wc') {
    let showL = false, showW = false, showC = false, idx = 1;
    while (idx < words.length && words[idx].startsWith('-')) {
      const f = words[idx].slice(1);
      if (!f || !/^[lwc]+$/.test(f)) return null;
      if (f.includes('l')) showL = true;
      if (f.includes('w')) showW = true;
      if (f.includes('c')) showC = true;
      idx++;
    }
    if (!showL && !showW && !showC) showL = showW = showC = true;
    if (idx >= words.length) return null;
    const argsList = words.slice(idx);
    const paths = argsList.map(resolveSafe);
    if (paths.some(p => !p)) return null;
    try {
      let out = '', totL = 0, totW = 0, totC = 0;
      const fmt = (l, w, c, name) => {
        const cols = [];
        if (showL) cols.push(String(l).padStart(7));
        if (showW) cols.push(String(w).padStart(7));
        if (showC) cols.push(String(c).padStart(7));
        return `${cols.join(' ')} ${name}\n`;
      };
      for (let i = 0; i < paths.length; i++) {
        const buf = fs.readFileSync(paths[i]);
        let l = 0;
        for (let j = 0; j < buf.length; j++) if (buf[j] === 10) l++;
        const trimmed = buf.toString('utf8').trim();
        const w = trimmed ? trimmed.split(/\s+/).length : 0;
        const c = buf.length;
        totL += l; totW += w; totC += c;
        out += fmt(l, w, c, argsList[i]);
      }
      if (paths.length > 1) out += fmt(totL, totW, totC, 'total');
      return ok(out);
    } catch (e) {
      return fail(`wc: ${e.message}\n`);
    }
  }
  return null;
}

function snapshot(root, protectedDir, signal, staging) {
  const files = Object.create(null);
  const todo = [''];
  let remaining = bytesLimit;
  let entries = 0;
  while (todo.length) {
    const dir = todo.pop();
    for (const name of fs.readdirSync(join(root, dir))) {
      signal.throwIfAborted();
      const key = dir ? dir + '/' + name : name;
      const path = join(root, key);
      if (path === staging || within(path, protectedDir) || key.split('/').some(s => s.startsWith('.'))) continue;
      if (!visible(key) || ++entries > entriesLimit) throw new Error('isolated shell workspace exceeds path or entry limit');
      const stat = fs.lstatSync(path);
      if (stat.isDirectory()) {
        files[key] = null;
        todo.push(key);
      } else if (stat.isFile() && stat.size <= remaining) {
        const bytes = fs.readFileSync(path);
        if (bytes.length !== stat.size) throw new Error('workspace changed during read');
        files[key] = bytes.toString('base64');
        remaining -= bytes.length;
      } else throw new Error('unsupported file or workspace exceeds 8 MiB');
    }
  }
  return files;
}

function exchange(job, timeoutMs, signal) {
  const body = Buffer.from(JSON.stringify(job));
  if (body.length > frameLimit) throw new Error('isolated shell request exceeds limit');
  return new Promise((accept, reject) => {
    const header = Buffer.alloc(4);
    header.writeUInt32BE(body.length);
    const socket = net.createConnection({ host: '10.0.2.103', port: 8766 });
    const timer = setTimeout(() => socket.destroy(new Error('isolated shell timed out')), timeoutMs + 90000);
    const abort = () => socket.destroy(new Error('isolated shell cancelled'));
    signal.addEventListener('abort', abort, { once: true });
    if (signal.aborted) abort();
    let length = 0;
    let expected;
    const parts = [];
    socket.on('connect', () => socket.write(Buffer.concat([header, body])));
    socket.on('data', buf => {
      length += buf.length;
      parts.push(buf);
      if (length > frameLimit + 4) return socket.destroy(new Error('oversized shell response'));
      if (expected === undefined && length >= 4) expected = Buffer.concat(parts, length).readUInt32BE();
      if (expected > frameLimit) return socket.destroy(new Error('invalid shell response size'));
      if (expected === undefined || length < expected + 4) return;
      try {
        if (length !== expected + 4) throw new Error('invalid shell frame');
        accept(JSON.parse(Buffer.concat(parts, length).subarray(4)));
      } catch (error) { reject(error); }
      socket.destroy();
    });
    socket.on('error', reject);
    socket.on('close', () => {
      clearTimeout(timer);
      signal.removeEventListener('abort', abort);
      reject(new Error('isolated shell disconnected; check LIXINOS_CODEX_SHELL=1'));
    });
  });
}

function publish(root, protectedDir, before, result, signal) {
  const changes = Object.entries(result.changed);
  const removed = result.removed;
  if (!changes.length && !removed.length) return;
  if (changes.length + removed.length > 2 * entriesLimit) throw new Error('too many shell changes');
  for (const key of [...changes.map(([key]) => key), ...removed]) {
    if (typeof key !== 'string' || !visible(key) || within(join(root, key), protectedDir))
      throw new Error('unsupported output path from isolated shell');
  }
  const stage = fs.mkdtempSync(join(root, '.dsh-job-'));
  try {
    let bytes = 0;
    const files = new Map();
    for (const [key, value] of changes) {
      if (value === null) continue;
      if (typeof value !== 'string') throw new Error('invalid shell output file');
      const data = Buffer.from(value, 'base64');
      bytes += data.length;
      if (bytes > bytesLimit || data.toString('base64') !== value) throw new Error('invalid shell file data');
      const path = join(stage, String(files.size));
      fs.writeFileSync(path, data, { flag: 'wx' });
      files.set(key, path);
    }
    volume.transaction(call => {
      const now = snapshot(root, protectedDir, signal, stage);
      if (Object.keys(before).length !== Object.keys(now).length ||
          Object.keys(before).some(key => before[key] !== now[key]))
        throw new Error('workspace changed while the shell ran; outputs were discarded');
      const toRemove = new Set(removed);
      for (const [key, value] of changes) {
        if (Object.hasOwn(before, key) && (value === null) !== (before[key] === null)) toRemove.add(key);
      }
      for (const key of [...toRemove].sort((a, b) => b.length - a.length))
        call(before[key] === null ? 259 : 3, join(root, key));
      for (const [key, value] of changes.sort(([a], [b]) => a.length - b.length)) {
        if (value === null) call(1, join(root, key));
        else call(5, files.get(key), join(root, key));
      }
      signal.throwIfAborted();
    });
  } finally { fs.rmSync(stage, { recursive: true, force: true }); }
}

export async function runWorkspace(args, policy, workdir, signal) {
  if (!['workspace-write', 'read-only'].includes(policy?.mode) || args.run_in_background)
    throw new Error('LixinOS shell supports foreground read-only and workspace-write commands');
  const root = fs.realpathSync(policy.workspaceRoot);
  const cwd = fs.realpathSync(resolve(workdir ?? root));
  const protectedDir = resolve(process.env.DSH_HOME ?? join(homedir(), '.dsh'));
  if (root === '/' || !within(cwd, root) || within(root, protectedDir))
    throw new Error('select a project directory within the shell workspace');
  const timeoutMs = Math.min(args.timeoutMs ?? 30000, 120000);
  const fast = tryReadonlyBuiltin(args.command, root, cwd, protectedDir, timeoutMs);
  if (fast) return fast;
  const files = snapshot(root, protectedDir, signal);
  const subdir = relative(root, cwd);
  if (subdir && files[subdir] !== null) throw new Error('working directory excluded from shell workspace');
  const result = await exchange({ files, cwd: subdir, cmd: args.command, timeout: Math.ceil(timeoutMs / 1000) }, timeoutMs, signal);
  if (result.error) throw new Error('isolated shell: ' + result.error);
  if (!Number.isInteger(result.status) || result.status < 0 || result.status > 255 ||
      typeof result.output !== 'string' || !result.changed || typeof result.changed !== 'object' ||
      Array.isArray(result.changed) || !Array.isArray(result.removed)) throw new Error('invalid isolated shell response');
  if (policy.mode === 'read-only' && (Object.keys(result.changed).length || result.removed.length))
    throw new Error('read-only shell discarded attempted workspace changes');
  signal.throwIfAborted();
  publish(root, protectedDir, files, result, signal);
  return { kind: 'foreground', cwd, exitCode: result.status, signal: null, timedOut: false, aborted: false,
    timeoutMs, stdout: { text: result.output, truncated: false }, stderr: { text: '', truncated: false } };
}
