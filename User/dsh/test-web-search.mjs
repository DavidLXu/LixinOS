// Check the packaged provider without an API key or a network connection.
import assert from 'node:assert/strict';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

const modules = resolve(process.argv[2], 'lib/dsh/node_modules/@deepseek-ai');
const { apply, DEEPSEEK_DEFAULT_MODEL } = await import(pathToFileURL(resolve(modules, 'dsh-web-search-deepseek/lib/index.js')));
const { createLaunchEnvironmentSnapshot } = await import(pathToFileURL(resolve(modules, 'dsh-launch-environment/lib/index.js')));
const official = 'https://api.deepseek.com/anthropic/v1/messages';
const relay = 'http://127.0.0.1:8999/anthropic';
const separate = 'https://search.example/v1';

async function check({ env = {}, user = {}, project = {}, overrides = {}, endpoint, failure }) {
  const environment = createLaunchEnvironmentSnapshot([
    { source: 'process', values: { DEEPSEEK_API_KEY: 'test', ...env } },
    { source: 'user-env', values: user },
    { source: 'project-env', values: project },
  ]);
  let provider;
  const ctx = {
    get: name => name === 'launchEnvironment' ? environment : undefined,
    web: { registerSearchProvider(value) { provider = value; } },
  };
  const settings = { apiKey: undefined, apiKeyEnv: 'DEEPSEEK_API_KEY', baseURL: undefined,
    model: DEEPSEEK_DEFAULT_MODEL, apiVersion: '2023-06-01', maxTokens: 4096, maxUses: 5, ...overrides };
  apply(ctx, Object.fromEntries(Object.keys(settings).map(k => [k, { get: () => settings[k] }])));
  const source = { type: 'web_search_result', title: 'Example', url: 'https://example.org/' };
  globalThis.fetch = async (url, request) => {
    assert.equal(url, endpoint);
    assert.equal(request.headers['x-api-key'], settings.apiKey ?? 'test');
    assert.equal(JSON.parse(request.body).tools[0].type, 'web_search_20250305');
    if (failure === 401) return Response.json({ error: 'Unauthorized' }, { status: 401 });
    return Response.json({ content: [{ type: 'web_search_tool_result', content: failure ?
      { type: 'web_search_tool_result_error', error_code: 'unavailable' } : [source] }] });
  };
  const result = provider.search({ query: 'example' });
  if (failure) await assert.rejects(result, failure === 401 ? /401/ : /unavailable/);
  else assert.equal((await result).sources[0].url, source.url);
}

await check({ endpoint: official });
for (const base of [relay, relay + '/', relay + '/v1/']) {
  await check({ env: { DEEPSEEK_BASE_URL: base }, endpoint: relay + '/v1/messages' });
}
await check({ env: { DEEPSEEK_BASE_URL: relay, DEEPSEEK_SEARCH_BASE_URL: separate + '/' }, endpoint: separate + '/messages' });
await check({ env: { DEEPSEEK_SEARCH_BASE_URL: separate }, overrides: { baseURL: relay + '/v1' }, endpoint: relay + '/v1/messages' });
await check({ env: { DEEPSEEK_BASE_URL: relay }, overrides: { apiKey: 'search-only' }, endpoint: official });
await check({ project: { DEEPSEEK_BASE_URL: relay }, endpoint: official });
await check({ user: { DEEPSEEK_BASE_URL: relay }, endpoint: relay + '/v1/messages' });
await check({ endpoint: official, failure: true });
await check({ endpoint: official, failure: 401 });
console.log('Packaged web search routing, source and failure checks passed.');
