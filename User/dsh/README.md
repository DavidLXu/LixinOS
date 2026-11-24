# DeepSeek Harness on LixinOS

The guest shell command `dsh` starts the upstream DeepSeek Harness CLI 0.2.1-rc.5 with the guest's ARM64 Node executable. Build the user package on the workstation with npm, Python 3, and the project's AArch64 cross compiler.

```sh
python3 User/dsh/package.py Kernel/target/dsh-port/dsh-release
python3 scripts/test-dsh.py --headless --package Kernel/target/dsh-port/dsh-release \
    --node /path/to/openssl-node-package --disk /path/to/test-seed.img
```

The packaging script verifies the upstream CLI tarball and uses the npm lockfile to install the base and headless bundles. The test makes a disposable copy of the supplied disk. To install the package, use the offline package installer with a powered-off volume. Install the Node package on the same volume.

```sh
python3 User/gcc/install.py --toolchain node /path/to/openssl-node-package /path/to/disk.img
python3 User/gcc/install.py --toolchain dsh Kernel/target/dsh-port/dsh-release /path/to/disk.img
```

For a model request, install the [OpenSSL build of Node](../node/README.md). In the guest, choose a writable `DSH_HOME`. DSH reads `DEEPSEEK_API_KEY` from its inherited environment or from `$DSH_HOME/.env`. Use the guest shell from the directory containing the file you want DSH to read.

```sh
export DSH_HOME=/scratch/dsh-user/.dsh
export DEEPSEEK_BASE_URL=https://api.deepseek.com/anthropic
dsh headless 'Read witness.txt and reply with its contents.'
```

The automated headless test runs without credentials. Its local Messages fixture checks a file read, an exclusive create, an edit, and the preceding conversation and tool results in a new process. It also runs a foreground shell command in an isolated guest and checks its output file on the primary volume.

Use the Node 22 OpenSSL build linked with the current LixinOS adapter for headless sessions. For example, start a task with `dsh headless --json 'Read brief.txt'`, then reuse the returned ID with `dsh headless --session-id SESSION_ID 'Implement that change'`. Earlier logs that were already truncated can only be recovered from an intact copy.

Foreground shell commands run in a disposable, networkless LixinOS VM. On custom disk launches, set `LIXINOS_CODEX_SHELL=1` on the Mac before starting QEMU. The worker copies a project directory (up to 1,024 entries and 8 MiB) into `/workspace`. Use project-relative test paths. Hidden entries and the DSH configuration directory are excluded. In workspace-write mode the guest publishes successful filesystem transactions after checking for concurrent edits. A command's exit code and output are returned even if its tests fail. Read-only mode rejects attempts to publish changes. Background shell jobs are unsupported.

The base Node 22.23.5 port runs `dsh --help` and `dsh --version`. The headless profile requires Node compiled with OpenSSL. The launcher enables internal module resolution in its own Node process for the Cordis plugin loader. On V8 Lite, DSH uses a Fetch implementation backed by Node's HTTP and HTTPS modules. Interactive TUI, web, plugin installation and desktop profiles are outside the tested package.

The JSONL session writer uses exclusive file creation for a writer claim. It releases the claim on normal session close. If the guest loses power, an existing session may retain a `session.lock.exclusive` file. To recover that session, remove its claim with all writers stopped. Fresh sessions use distinct directories.
