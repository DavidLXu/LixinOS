// Transactions must finish synchronously, before another Node task can use the mailbox.
function transaction(operation) {
  const { batch } = process._linkedBinding('lixinos_fs');
  const call = (op, path = '', other = '') => batch(op, path, other);
  call(0);
  try {
    operation(call);
    call(6);
  } catch (error) {
    call(7);
    throw error;
  }
}

function createFromStage(source, target) {
  transaction(call => {
    if (call(8, target)) {
      throw Object.assign(new Error('destination already exists'), { code: 'EEXIST' });
    }
    call(5, source, target);
  });
}

module.exports = { transaction, createFromStage };
