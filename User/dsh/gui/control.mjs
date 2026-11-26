// GUI-only Cordis overlay. Durable replies still come from dsh headless --json.
import { existsSync } from 'node:fs';

export const name = 'lixinos-window-controls';
export const inject = ['systemPrompt'];
export function apply(ctx) {
  const flag = process.env.LIXINOS_DSH_STOP_FILE;
  if (!flag) return;
  ctx.systemPrompt.context({
    name: 'lixinos:search-date',
    order: ctx.systemPrompt.getContextOrder('WEB_SURFACE'),
    required: true,
    text: () => `Current UTC date: ${new Date().toISOString().slice(0, 10)}. When checking current facts, use current sources. If web search fails or returns no sources, say so.`,
  });
  let running, stopped = false;
  const emit = (type, text = '') => process.stdout.write(JSON.stringify({type, text}) + '\n');
  ctx.on('agent/status', ({agent, status}) => {
    if (agent.session.header.parentSession || agent.session.header.origin === 'subagent') return;
    running = status === 'running' ? agent : undefined;
  });
  ctx.on('agent/assistant-stream', ({agent, frame}) => {
    if (agent !== running) return;
    if (frame.type === 'start' || frame.type === 'end') emit('preview_reset');
    else if (frame.chunk?.type === 'text-delta') emit('preview_text', frame.chunk.text);
  });
  const timer = setInterval(() => {
    if (!running || stopped || !existsSync(flag)) return;
    stopped = true;
    running.cancel({kind: 'user'});
    emit('stop_accepted');
  }, 200);
  timer.unref();
  ctx.on('dispose', () => clearInterval(timer));
}
