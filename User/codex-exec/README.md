# Codex for LixinOS

The AArch64 guest runs OpenAI's Rust `codex-tui` and `codex-exec` crates at `rust-v0.171.0`. With the daily disk configured for DeepSeek, start either kernel and run these commands in the guest.

```sh
codex
codex --sandbox workspace-write "Compile main.c and run its tests"
codex line
codex --version
codex exec --skip-git-repo-check --ephemeral "Say hello"
codex exec --skip-git-repo-check --ephemeral -o answer.txt "Read notes.txt and summarize it"
cat notes.txt | codex exec --skip-git-repo-check --ephemeral --json - > result.jsonl
```

Running `codex` opens the upstream fullscreen composer and transcript. Type a prompt and press Enter. Type `/` to open the command menu, or submit `/exit` to return to the shell. The native console adapter delivers keypresses, bracketed pastes and size changes to Crossterm. The TUI uses the configured provider and the same guest workspace tools as `codex exec`. Sessions use in-memory history because LixinOS lacks the filesystem operations needed by Codex’s persistent thread store. Files saved to the workspace survive exit. The package includes the upstream system skills under `/lib/codex/skills` so first launch can read them directly.

For the simpler line interface, run `codex line`. Enter `/help` for its keys.

`-o` writes the final assistant message. The guest stages the answer, checks the original destination's contents and timestamps, and publishes the answer in a filesystem transaction. A concurrent change to the destination causes a write error. Shell redirection can capture the JSON event stream separately.

## Running shell commands and tests

From a project directory, allow workspace changes to keep build products or edits after a tool call.

```sh
codex exec --sandbox workspace-write --skip-git-repo-check --ephemeral "Compile main.c with tcc and run the result"
```

The launcher enables the shell worker for the daily disk. For a trusted custom disk, set `LIXINOS_CODEX_SHELL=1` on the Mac before launch. Set it to `0` to disable the worker. Literal `pwd`, `ls` and `cat` retain their direct guest implementations.

Other commands run inside a fresh LixinOS VM using the matching released kernel flavor, the stock guest tools and TinyCC. The VM has no network device, API relay, shared host directory or daily disk. Its shell supports pipelines, redirection, sequencing, `&&` and `||`. Use project-relative paths. The allowed workspace is mounted at `/workspace`; the selected working directory is preserved beneath it. Use `sh tests.sh` or invoke a script with a `#!/bin/sh` or `#!/bin/bash` header. The shell supports foreground, noninteractive commands. Each job starts with a fresh environment and private `/tmp`.

Codex exports readable project entries, omitting `.git`, `.codex` and `.agents`. It checks workspace changes against both the turn's and the command's filesystem permissions. It checks the original contents of touched files and publishes creates, edits and removals in one guest filesystem transaction. Read-only sessions discard workspace writes and report an error. Limits per job are 1,024 workspace entries, 8 MiB of file contents, 64 KiB of console output, and 512 bytes per shell segment. Programs installed separately on the daily disk must be supplied in the project or added to the worker toolchain to run there.

## DeepSeek credentials

The guest uses `deepseek-flash` through a VM-private HTTP endpoint. The Mac reads the generic Keychain item with service `LixinOS.DeepSeek` and the current macOS account, then sends the Responses request to `https://api.deepseek.com/responses` over TLS. Only the Mac receives the API key. Guest programs on a bridge-enabled disk can use this endpoint to make billed Responses requests.

The launcher enables this relay for the daily disk. Set `LIXINOS_CODEX_DEEPSEEK=0` when launching to turn it off, or set it to `1` to enable it for a trusted custom disk. The standard Codex OpenAI provider and manually configured guest providers remain available. [DeepSeek's Codex integration](https://api-docs.deepseek.com/quick_start/agent_integrations/codex/)

## Build and install on macOS

Use the musl sysroot, OpenSSL archives and Arm GNU cross compiler prepared by the CPython and GCC ports. The build uses Rust 1.100.0 with the `aarch64-unknown-linux-musl` standard library. Run from the repository root and shut down the guest before installing.

```sh
export LIXINOS_CODEX_BUILD="$HOME/Library/Caches/LixinOS/codex-exec-0.171.0"
User/codex-exec/build.sh
python3 User/codex-exec/package.py "$LIXINOS_CODEX_BUILD" "$LIXINOS_CODEX_BUILD/package"
python3 User/gcc/install.py --toolchain codex "$LIXINOS_CODEX_BUILD/package" data/shared.img
python3 User/codex-exec/configure.py data/shared.img
```

The provider installer creates `/home/lixin/.codex/config.toml`. If the guest has a custom configuration, merge the provider settings from `deepseek.toml` in this directory.

## Verification

The smoke test boots a disposable primary disk. Install the Python package `pyte` in the test environment for `--tui`, which checks the text displayed after incremental terminal updates. Its Responses fixture builds and runs a native C test, verifies its output and exit status, checks saved build files and removals, and exercises read-only mode and concurrent final-message writes. Add `--live` to repeat a build and run with DeepSeek using the Mac Keychain item and a synthetic source file.

```sh
python3 User/codex-exec/smoke.py "$LIXINOS_CODEX_BUILD/package" --kernel rust
python3 User/codex-exec/smoke.py "$LIXINOS_CODEX_BUILD/package" --kernel tcc
python3 User/codex-exec/smoke.py "$LIXINOS_CODEX_BUILD/package" --kernel rust --interactive
python3 User/codex-exec/smoke.py "$LIXINOS_CODEX_BUILD/package" --kernel tcc --tui
python3 User/codex-exec/smoke.py "$LIXINOS_CODEX_BUILD/package" --kernel rust --tui --live
```

The pinned source is the [OpenAI Codex exec crate](https://github.com/openai/codex/tree/295eca93394bb83a6ec8e0841033f3997f97fcbd/codex-rs/exec).
