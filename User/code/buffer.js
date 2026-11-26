// VS Code's UTF-16 offsets match QTextDocument's cursor offsets.
import { PieceTreeBase, StringBuffer, createLineStartsFast } from
    'monaco-editor/esm/vs/editor/common/model/pieceTreeTextBuffer/pieceTreeBase.js';

let tree, endings, defaultEnding;
globalThis.resetDocument = text => {
    endings = text.match(/\r\n|\r|\n/g) || [];
    defaultEnding = endings[0] || '\n';
    const normalized = text.replace(/\r\n|\r/g, '\n');
    const chunk = new StringBuffer(normalized, createLineStartsFast(normalized));
    tree = new PieceTreeBase([chunk], '\n', true);
    return normalized;
};
globalThis.changeDocument = (offset, removed, inserted) => {
    const start = tree.getPositionAt(offset).lineNumber - 1;
    const end = tree.getPositionAt(offset + removed).lineNumber - 1;
    const count = inserted.split('\n').length - 1;
    if (end !== start || count) {
        endings = endings.slice(0, start).concat(Array(count).fill(defaultEnding), endings.slice(end));
    }
    if (removed) tree.delete(offset, removed);
    if (inserted.length) tree.insert(offset, inserted, true);
};
globalThis.documentText = () => {
    const snapshot = tree.createSnapshot('');
    const chunks = [];
    let line = 0;
    for (let chunk; (chunk = snapshot.read()) !== null;) {
        chunks.push(chunk.replace(/\n/g, () => endings[line++]));
    }
    return chunks.join('');
};
