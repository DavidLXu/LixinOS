// V8 Lite lacks ICU's Unicode property tables. Expand the pinned upstream
// patterns while packaging them, keeping Unicode mode and astral code points.
const fs = require('node:fs');
const path = require('node:path');
const {parse} = require('acorn');
const rewrite = require('regexpu-core');

const property = /\\[pP]\{/;
function transform(file) {
  const source = fs.readFileSync(file, 'utf8');
  if (!property.test(source)) return;
  const ast = parse(source, {ecmaVersion: 'latest', sourceType: 'module', allowReturnOutsideFunction: true});
  const edits = [];
  function visit(node, parent) {
    if (!node || typeof node !== 'object') return;
    if (node.regex && property.test(node.regex.pattern)) {
      const {pattern, flags} = node.regex;
      const result = rewrite(pattern, flags, {unicodePropertyEscapes: 'transform'});
      edits.push([node.start, node.end, '/' + result + '/' + flags]);
      return;
    }
    const pattern = node.type === 'Literal' ? node.value :
      node.type === 'TemplateLiteral' && !node.expressions.length ? node.quasis[0].value.cooked : null;
    if (typeof pattern === 'string' && property.test(pattern)) {
      // The packages also construct a few patterns from strings (Zod and Execa).
      const constructor = parent?.callee?.name === 'RegExp' && parent.arguments[0] === node;
      const emoji = parent?.type === 'VariableDeclarator' &&
        ['_emoji', '_emojiRegex', 'emojiPattern'].includes(parent.id.name);
      if (!constructor && !emoji) throw Error('unknown dynamic Unicode pattern in ' + file);
      const flags = constructor ? parent.arguments[1]?.value : 'u';
      const result = rewrite(pattern, flags, {unicodePropertyEscapes: 'transform'});
      edits.push([node.start, node.end, JSON.stringify(result)]);
      return;
    }
    for (const child of Object.values(node)) {
      if (Array.isArray(child)) child.forEach(item => visit(item, node));
      else if (child?.type) visit(child, node);
    }
  }
  visit(ast);
  let output = source;
  for (const [start, end, replacement] of edits.reverse()) {
    output = output.slice(0, start) + replacement + output.slice(end);
  }
  if (edits.length) fs.writeFileSync(file, output);
}
function walk(dir) {
  for (const entry of fs.readdirSync(dir, {withFileTypes: true})) {
    const file = path.join(dir, entry.name);
    if (entry.isDirectory()) walk(file);
    else if (entry.isFile() && /\.[cm]?js$/.test(entry.name)) transform(file);
  }
}
walk(process.argv[2]);
