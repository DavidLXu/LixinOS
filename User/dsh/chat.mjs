// Resident multi-turn interactive terminal client for DeepSeek Harness on LixinOS.
// Keeps Cordis and the live Agent in memory across turns (zero Node/plugin cold starts).
import { randomUUID } from 'node:crypto';
import { readdirSync, readFileSync, existsSync } from 'node:fs';
import { join } from 'node:path';
import readline from 'node:readline';
import { brandString } from '@deepseek-ai/dsh-brand';
import { installModelSelection } from '@deepseek-ai/dsh-agent';
import { createUserMessage } from '@deepseek-ai/dsh-llm';
import { resolveDshHome } from '@deepseek-ai/dsh-home-paths';

export const name = 'lixinos-chat-runner';
export const inject = ['agentDefaultModel', 'agents', 'sessions', 'cmdlineArgs'];

const DIM = '\x1b[2m';
const BOLD = '\x1b[1m';
const CYAN = '\x1b[36m';
const YELLOW = '\x1b[33m';
const RED = '\x1b[31m';
const RESET = '\x1b[0m';

const HELP = [
  'Commands:',
  '  /help           Show this help',
  '  /sessions       List saved sessions in DSH_HOME',
  '  /resume <id>    Switch to an existing session ID',
  '  /new            Start a fresh session',
  '  /exit, /quit    Flush session and leave (or Ctrl-D)',
  '  Ctrl-C          Cancel the running turn (press twice to exit)',
].join('\n');

function listSavedSessions() {
  const root = join(resolveDshHome(), 'sessions');
  if (!existsSync(root)) return [];
  const items = [];
  for (const name of readdirSync(root)) {
    const file = join(root, name, 'events.jsonl');
    if (!existsSync(file)) continue;
    try {
      const firstLine = readFileSync(file, 'utf8').split('\n', 1)[0];
      const header = firstLine ? JSON.parse(firstLine) : {};
      items.push({ id: name, cwd: header.cwd ?? '', createdAt: header.createdAt ?? '' });
    } catch {
      items.push({ id: name, cwd: '', createdAt: '' });
    }
  }
  return items.sort((a, b) => String(b.createdAt).localeCompare(String(a.createdAt)));
}

function parseArgs(argv) {
  let sessionId;
  const promptParts = [];
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === '-h' || a === '--help') return { help: true };
    if ((a === '--session-id' || a === '-s' || a === '--resume') && i + 1 < argv.length) {
      sessionId = argv[++i];
    } else if (a === '--') {
      promptParts.push(...argv.slice(i + 1));
      break;
    } else if (!a.startsWith('-')) {
      promptParts.push(a);
    }
  }
  return { help: false, sessionId, initialPrompt: promptParts.join(' ').trim() || undefined };
}

export function apply(ctx) {
  const exit = ctx.get('appExit');
  const argv = ctx.get('cmdlineArgs')?.get() ?? [];
  const parsed = parseArgs(argv);
  if (parsed.help) {
    process.stdout.write('Usage: dsh chat [--session-id <id>] [initial prompt]\n\n' + HELP + '\n');
    exit?.(0);
    return;
  }

  let currentAgent;
  let running = false;
  let closed = false;

  const ensureAgent = async (requestedId) => {
    const agents = ctx.get('agents');
    const sessions = ctx.get('sessions');
    const defaultModel = ctx.get('agentDefaultModel');
    const fsService = ctx.get('fs');
    const cwd = fsService ? fsService.processPath(await fsService.resolve('.')) : process.cwd();
    const selection = defaultModel.currentSelection();
    const agentOptions = { provider: selection.provider, model: selection.model };
    const setup = (agentCtx) => {
      installModelSelection(agentCtx, { current: selection, assembled: void 0 });
    };
    if (currentAgent && (!requestedId || currentAgent.id === requestedId)) return currentAgent;
    if (currentAgent) {
      await sessions.flush(currentAgent.session).catch(() => {});
      currentAgent = undefined;
    }
    if (requestedId) {
      const sid = brandString(requestedId);
      const { agent } = await agents.resume({ resumeSessionId: sid, agentOptions, setup });
      await agent.whenIdle();
      currentAgent = agent;
      return agent;
    }
    const sid = brandString(`session-${randomUUID()}`);
    const { agent } = await agents.create({ sessionId: sid, meta: { cwd }, agentOptions, setup });
    await agent.whenIdle();
    currentAgent = agent;
    return agent;
  };

  const runTurn = async (prompt) => {
    const agent = await ensureAgent();
    const sessions = ctx.get('sessions');
    const subagents = ctx.get('subagents');
    running = true;
    let inReasoning = false;
    let inText = false;
    let sawOutput = false;
    let failure;

    const closeReasoning = () => {
      if (inReasoning) { process.stderr.write(RESET + '\n'); inReasoning = false; }
    };
    const stopStream = ctx.on('agent/assistant-stream', ({ agent: subject, frame }) => {
      if (subject !== agent) return;
      if (frame.type === 'start' || frame.type === 'end') {
        closeReasoning();
        if (inText) { process.stdout.write('\n'); inText = false; }
        return;
      }
      const chunk = frame.chunk;
      if (chunk.type === 'reasoning-delta' && chunk.text) {
        if (!inReasoning) { process.stderr.write(`${DIM}thinking: `); inReasoning = true; }
        process.stderr.write(chunk.text);
      } else if (chunk.type === 'text-delta' && chunk.text) {
        closeReasoning();
        if (!inText) { inText = true; sawOutput = true; }
        process.stdout.write(chunk.text);
      }
    });
    const stopEvents = ctx.on('session/event', (session, event) => {
      if (session !== agent.session) return;
      if (event.type === 'tool/call') {
        closeReasoning();
        if (inText) { process.stdout.write('\n'); inText = false; }
        process.stderr.write(`${YELLOW}• ${event.data.name}${RESET}\n`);
      } else if (event.type === 'tool/result' && event.surfaceOp === 'append' && event.data.message.isError) {
        process.stderr.write(`${RED}  tool error${RESET}\n`);
      }
    });
    const stopErrors = ctx.on('agent/error', ({ agent: subject, error }) => {
      if (subject === agent) failure = error;
    });
    try {
      agent.followup(createUserMessage({
        content: [{ type: 'text', text: prompt }],
        source: { kind: 'user' },
      }));
      while (true) {
        await agent.whenIdle();
        const idleSeq = agent.session.seq;
        if (!await subagents?.waitForChildren(agent) && agent.status === 'idle' && agent.session.seq === idleSeq) break;
      }
      await sessions.flush(agent.session);
      if (failure) {
        process.stderr.write(`${RED}Error: ${failure instanceof Error ? failure.message : String(failure)}${RESET}\n`);
      } else if (!sawOutput) {
        const last = agent.session.seq > 0 ? agent.session.eventAt(agent.session.seq - 1) : undefined;
        if (last?.type === 'turn/end' && last.data.reason?.kind === 'cancelled') {
          process.stderr.write(`${DIM}Turn cancelled.${RESET}\n`);
        }
      }
    } finally {
      closeReasoning();
      if (inText) process.stdout.write('\n');
      stopStream();
      stopEvents();
      stopErrors();
      running = false;
    }
  };

  (async () => {
    await ctx.get('loader')?.await();
    const selection = ctx.get('agentDefaultModel').currentSelection();
    process.stderr.write(`${BOLD}DeepSeek Harness${RESET} ${DIM}| ${selection.model} | ${process.cwd()}${RESET}\n${DIM}Type /help for commands. Session persists automatically.${RESET}\n`);
    if (parsed.sessionId) {
      await ensureAgent(parsed.sessionId);
      process.stderr.write(`${DIM}Resumed ${currentAgent.id}${RESET}\n`);
    }
    if (parsed.initialPrompt) {
      await runTurn(parsed.initialPrompt);
    }

    const rl = readline.createInterface({
      input: process.stdin,
      output: process.stderr,
      prompt: `${CYAN}dsh>${RESET} `,
      terminal: Boolean(process.stdin.isTTY),
    });

    rl.on('SIGINT', () => {
      if (running && currentAgent) {
        currentAgent.cancel({ kind: 'user' });
        process.stderr.write(`\n${DIM}Cancelling…${RESET}\n`);
      } else {
        rl.close();
      }
    });

    const promptNext = () => { if (!closed) rl.prompt(); };
    rl.on('line', async (raw) => {
      const line = raw.trim();
      if (!line) return promptNext();
      if (line === '/exit' || line === '/quit') { rl.close(); return; }
      if (line === '/help') { process.stderr.write(HELP + '\n'); return promptNext(); }
      if (line === '/new') {
        if (currentAgent) {
          await ctx.get('sessions').flush(currentAgent.session).catch(() => {});
          currentAgent = undefined;
        }
        process.stderr.write(`${DIM}Started fresh session.${RESET}\n`);
        return promptNext();
      }
      if (line === '/sessions') {
        const list = listSavedSessions();
        if (!list.length) process.stderr.write(`${DIM}No saved sessions.${RESET}\n`);
        for (const s of list.slice(0, 20)) {
          const mark = currentAgent?.id === s.id ? '*' : ' ';
          process.stderr.write(`${mark} ${s.id}  ${DIM}${s.cwd}${RESET}\n`);
        }
        return promptNext();
      }
      if (line.startsWith('/resume ')) {
        const sid = line.slice(8).trim();
        try {
          await ensureAgent(sid);
          process.stderr.write(`${DIM}Resumed ${currentAgent.id}${RESET}\n`);
        } catch (err) {
          process.stderr.write(`${RED}${err instanceof Error ? err.message : String(err)}${RESET}\n`);
        }
        return promptNext();
      }
      rl.pause();
      try {
        await runTurn(line);
      } catch (err) {
        process.stderr.write(`${RED}Error: ${err instanceof Error ? err.message : String(err)}${RESET}\n`);
      }
      rl.resume();
      promptNext();
    });

    rl.on('close', async () => {
      if (closed) return;
      closed = true;
      if (currentAgent) {
        await ctx.get('sessions').flush(currentAgent.session).catch(() => {});
        process.stderr.write(`${DIM}Session saved: ${currentAgent.id}${RESET}\n`);
      }
      exit?.(0);
    });

    promptNext();
  })().catch((err) => {
    process.stderr.write(`dsh chat: ${err instanceof Error ? err.message : String(err)}\n`);
    exit?.(1);
  });
}
