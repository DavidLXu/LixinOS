// Resident persistent Cordis worker for the LixinOS DSH GUI.
// Keeps Node, Cordis plugins, and live Agents warm across turns so turn 2+
// start in milliseconds and Stop cancels immediately over stdin.
import { randomUUID } from 'node:crypto';
import readline from 'node:readline';
import { brandString } from '/lib/dsh/node_modules/@deepseek-ai/dsh-brand/lib/index.js';
import { installModelSelection } from '/lib/dsh/node_modules/@deepseek-ai/dsh-agent/lib/index.js';
import { createUserMessage } from '/lib/dsh/node_modules/@deepseek-ai/dsh-llm/lib/index.js';
import { SessionSeq } from '/lib/dsh/node_modules/@deepseek-ai/dsh-session/lib/index.js';

export const name = 'lixinos-gui-worker';
export const inject = ['systemPrompt', 'agentDefaultModel', 'agents', 'sessions'];

const MAX_STR = 8192;

function clipStr(s, limit = MAX_STR) {
  if (typeof s !== 'string' || Buffer.byteLength(s, 'utf8') <= limit) return { value: s, cut: false };
  let out = Buffer.from(s, 'utf8').subarray(0, limit).toString('utf8');
  if (out.endsWith('\uFFFD')) out = out.slice(0, -1);
  return { value: out, cut: true };
}

function parseArgs(raw) {
  if (!raw) return {};
  try { return JSON.parse(raw); } catch { return raw; }
}

function resultText(blocks) {
  if (!Array.isArray(blocks)) return '';
  return blocks.filter(b => b && b.type === 'text' && typeof b.text === 'string').map(b => b.text).join('');
}

function extractUsage(data) {
  if (data?.usage) return data.usage;
  if (Array.isArray(data?.stream)) {
    for (let i = data.stream.length - 1; i >= 0; i--) {
      if (data.stream[i]?.type === 'usage' && data.stream[i].usage) return data.stream[i].usage;
    }
  }
  return undefined;
}

function summarize(session, firstSeq) {
  let started = false;
  let text = '';
  let reason;
  const length = session.seq;
  for (let seq = firstSeq; seq < length; seq++) {
    const ev = session.eventAt(SessionSeq(seq));
    if (!ev) continue;
    if (ev.type === 'turn/start') { started = true; continue; }
    if (!started) continue;
    if (ev.type === 'assistant/message') {
      const joined = ev.data.message.content.filter(b => b.type === 'text').map(b => b.text).join('');
      if (joined !== '') text = joined;
    }
    if (ev.type === 'turn/end') reason = ev.data.reason;
  }
  return { text, reason };
}

export function apply(ctx) {
  ctx.systemPrompt.context({
    name: 'lixinos:search-date',
    order: ctx.systemPrompt.getContextOrder('WEB_SURFACE'),
    required: true,
    text: () => `Current UTC date: ${new Date().toISOString().slice(0, 10)}. When checking current facts, use current sources. If web search fails or returns no sources, say so.`,
  });

  const emit = (obj) => {
    process.stdout.write(JSON.stringify(obj) + '\n');
  };

  const liveAgents = new Map();
  let readyPromise;
  let turnQueue = Promise.resolve();
  let activeAgent = null;
  let cancelRequested = false;
  let stopEmitted = false;
  let inTurn = false;
  let shuttingDown = false;

  const emitStopOnce = () => {
    if (stopEmitted) return;
    stopEmitted = true;
    emit({ type: 'stop_accepted' });
  };

  const getOrResumeAgent = async (requestedId) => {
    const agents = ctx.get('agents');
    const defaultModel = ctx.get('agentDefaultModel');
    const fsService = ctx.get('fs');
    const cwd = fsService ? fsService.processPath(await fsService.resolve('.')) : process.cwd();
    const selection = defaultModel.currentSelection();
    const agentOptions = { provider: selection.provider, model: selection.model };
    const setup = (agentCtx) => {
      installModelSelection(agentCtx, { current: selection, assembled: void 0 });
    };
    if (requestedId) {
      if (liveAgents.has(requestedId)) {
        return { agent: liveAgents.get(requestedId), cwd };
      }
      const existing = agents.get(requestedId);
      if (existing) {
        liveAgents.set(requestedId, existing);
        return { agent: existing, cwd };
      }
      const sid = brandString(requestedId);
      const { agent } = await agents.resume({ resumeSessionId: sid, agentOptions, setup });
      await agent.whenIdle();
      liveAgents.set(agent.id, agent);
      return { agent, cwd };
    }
    const sid = brandString(`session-${randomUUID()}`);
    const { agent } = await agents.create({ sessionId: sid, meta: { cwd }, agentOptions, setup });
    await agent.whenIdle();
    liveAgents.set(agent.id, agent);
    return { agent, cwd };
  };

  const executeTurn = async (req) => {
    await readyPromise;
    inTurn = true;
    // Preserve any cancel that arrived while waiting for boot/queue!
    stopEmitted = false;
    let exitCode = 0;
    let agent;
    let cwd = process.cwd();
    try {
      const prompt = typeof req.prompt === 'string' ? req.prompt : '';
      if (!prompt.trim()) throw new Error('empty prompt');
      const resolved = await getOrResumeAgent(req.sessionId || '');
      agent = resolved.agent;
      cwd = resolved.cwd;
      emit({ type: 'session', sessionId: agent.id, cwd });

      if (cancelRequested) {
        emitStopOnce();
        return;
      }

      activeAgent = agent;
      const firstSeq = agent.session.seq;
      let stepUsage;
      let failure;
      let lastThinkingSent = 0;

      const stopStatus = ctx.on('agent/status', ({ agent: subject, status }) => {
        if (subject !== agent) return;
        if (status === 'running' && cancelRequested) {
          agent.cancel({ kind: 'user' });
          emitStopOnce();
        }
      });

      const stopStream = ctx.on('agent/assistant-stream', ({ agent: subject, frame }) => {
        if (subject !== agent) return;
        if (frame.type === 'start' || frame.type === 'end') {
          emit({ type: 'preview_reset', text: '' });
        } else if (frame.chunk?.type === 'text-delta' && frame.chunk.text) {
          emit({ type: 'preview_text', text: frame.chunk.text });
        } else if (frame.chunk?.type === 'reasoning-delta' && frame.chunk.text) {
          const now = Date.now();
          if (now - lastThinkingSent >= 180) {
            lastThinkingSent = now;
            emit({ type: 'preview_thinking', text: '' });
          }
        }
      });

      const stopSession = ctx.on('session/event', (session, ev) => {
        if (session !== agent.session) return;
        switch (ev.type) {
          case 'turn/start':
            emit({ type: 'status', phase: 'turn_start', turn: ev.data.turn });
            break;
          case 'step/start':
            stepUsage = undefined;
            emit({ type: 'status', phase: 'step_start', turn: ev.data.turn, step: ev.data.step });
            break;
          case 'assistant/attempt': {
            const u = extractUsage(ev.data);
            if (u) stepUsage = u;
            break;
          }
          case 'assistant/message': {
            const u = extractUsage(ev.data);
            if (u) stepUsage = u;
            for (const block of ev.data.message.content) {
              if (block.type === 'reasoning') {
                const c = clipStr(block.text);
                emit({ type: 'thinking', text: c.value, ...(c.cut ? { truncated: true } : {}) });
              } else if (block.type === 'text') {
                const c = clipStr(block.text);
                emit({ type: 'text', text: c.value, ...(c.cut ? { truncated: true } : {}) });
              }
            }
            break;
          }
          case 'step/end':
            emit({
              type: 'status', phase: 'step_end', turn: ev.data.turn, step: ev.data.step,
              ...(stepUsage ? { usage: stepUsage } : {}),
            });
            stepUsage = undefined;
            break;
          case 'turn/end':
            emit({ type: 'status', phase: 'turn_end', turn: ev.data.turn, reason: ev.data.reason });
            break;
          case 'tool/call':
            emit({
              type: 'tool_call', callId: ev.data.callId, tool: ev.data.name,
              input: parseArgs(ev.data.arguments),
            });
            break;
          case 'tool/result':
            if (ev.surfaceOp === 'append') {
              const msg = ev.data.message;
              const c = clipStr(resultText(msg.content));
              emit({
                type: 'tool_result', callId: msg.toolCallId,
                status: msg.isError === true ? 'error' : 'completed',
                result: c.value, ...(c.cut ? { truncated: true } : {}),
              });
            }
            break;
        }
      });

      const stopErrors = ctx.on('agent/error', ({ agent: subject, error }) => {
        if (subject === agent) failure = { error, atOffset: agent.session.seq };
      });
      const stopTerminals = ctx.on('session/event', (session, ev) => {
        if (session === agent.session && ev.type === 'turn/end' && failure && ev.seq >= failure.atOffset) {
          failure = undefined;
        }
      });

      try {
        agent.followup(createUserMessage({
          content: [{ type: 'text', text: prompt }],
          source: { kind: 'user' },
        }));
        if (cancelRequested) {
          agent.cancel({ kind: 'user' });
          emitStopOnce();
        }
        const subagents = ctx.get('subagents');
        while (true) {
          await agent.whenIdle();
          const idleSeq = agent.session.seq;
          if (!await subagents?.waitForChildren(agent) && agent.status === 'idle' && agent.session.seq === idleSeq) break;
        }
        await ctx.get('sessions').flush(agent.session);
        if (failure && !cancelRequested) throw failure.error;
        const outcome = summarize(agent.session, firstSeq);
        if (!cancelRequested) {
          emit({ type: 'final', text: outcome.text });
          if (outcome.reason?.kind === 'error') {
            emit({ type: 'error', message: `${outcome.reason.error.code}: ${outcome.reason.error.message}` });
            exitCode = 1;
          }
        }
      } finally {
        stopStatus();
        stopStream();
        stopSession();
        stopErrors();
        stopTerminals();
        activeAgent = null;
      }
    } catch (error) {
      if (!cancelRequested) {
        const message = error instanceof Error ? error.message : String(error);
        emit({ type: 'error', message });
        exitCode = 1;
      }
    } finally {
      const wasCancelled = cancelRequested;
      cancelRequested = false;
      inTurn = false;
      emit({ type: 'turn_done', exitCode: wasCancelled ? 0 : exitCode });
    }
  };

  readyPromise = (async () => {
    await ctx.get('loader')?.await();
    emit({ type: 'worker_ready' });
  })();
  readyPromise.catch((err) => {
    emit({ type: 'error', message: err instanceof Error ? err.message : String(err) });
    ctx.get('appExit')?.(1);
  });

  const rl = readline.createInterface({ input: process.stdin, crlfDelay: Infinity });
  rl.on('line', (line) => {
    if (!line.trim() || shuttingDown) return;
    let msg;
    try { msg = JSON.parse(line); } catch { return; }
    if (msg.type === 'cancel') {
      cancelRequested = true;
      if (activeAgent) {
        activeAgent.cancel({ kind: 'user' });
        emitStopOnce();
      }
    } else if (msg.type === 'turn') {
      cancelRequested = false;
      turnQueue = turnQueue.then(() => executeTurn(msg));
    } else if (msg.type === 'shutdown') {
      shuttingDown = true;
      if (activeAgent) activeAgent.cancel({ kind: 'user' });
      rl.close();
    }
  });

  rl.on('close', () => {
    shuttingDown = true;
    turnQueue.finally(async () => {
      const sessions = ctx.get('sessions');
      for (const a of liveAgents.values()) {
        await sessions?.flush(a.session).catch(() => {});
      }
      ctx.get('appExit')?.(0);
    });
  });

  ctx.on('dispose', () => {
    shuttingDown = true;
    rl.close();
  });
}
