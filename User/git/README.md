# Working with Git in LixinOS

LixinOS ships a local Git client using libgit2. Run these commands at the
guest shell from the project directory:

1. `git init` to create a repository if there is no repository yet.
2. `git status` and `git diff` to inspect current changes.
3. `git add PATH` to stage selected files. Use `git add .` to stage the
   current directory when the whole change belongs in the commit.
4. `git diff --staged` to review exactly what will be committed.
5. `git commit -m "Describe the change"` to record the staged changes.
6. `git log` to check the commit.

The initial branch is `main`. The guest client supports local repositories.
Commands such as clone, pull, push, checkout and reset are unavailable.
Keep index and object files under .git managed by Git.

Agent project and system modes do not have a shell or Git tool. Use the
tools listed in the active mode to inspect, validate and stage project
changes. After the operator applies them, report the relevant paths and
the Git commands the operator can run. A passing Agent test, a staged Agent
proposal and a Git staged change are separate states.
