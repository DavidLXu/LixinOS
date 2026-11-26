// Node's built-in Fetch loads a WebAssembly HTTP parser (V8 Lite lacks Wasm)
// and Undici's socket pooling stalls on LixinOS's guest TCP stack. Use the
// native node:http/https transport lazily on LixinOS so offline CLI works too.
if (process.versions.openssl && (typeof WebAssembly === 'undefined' || process.platform === 'lixinos')) {
  // Importing node:http from ESM enumerates its exports, including three
  // WebSocket getters backed by Undici. Headless uses the HTTP client.
  const http = require('node:http');
  for (const name of ['WebSocket', 'CloseEvent', 'MessageEvent']) {
    delete http[name];
  }
  for (const name of ['fetch', 'Headers', 'Request', 'Response', 'FormData']) {
    // Delete V8's lazy data property before redefining it; reading its old
    // descriptor can initialize Undici.
    delete globalThis[name];
    Object.defineProperty(globalThis, name, {
      configurable: true,
      get() {
        const implementation = require('@whatwg-node/node-fetch');
        const value = name === 'fetch' ? async function fetch(input, options) {
          const url = new URL(input?.url ?? input);
          if (url.protocol === 'file:') throw new TypeError('fetch does not support file URLs');
          return implementation.fetch(input, options);
        } : implementation[name];
        Object.defineProperty(globalThis, name, { configurable: true, writable: true, value });
        return value;
      },
    });
  }
}
