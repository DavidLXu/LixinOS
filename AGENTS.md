# Kernel releases

For each completed iteration that changes either kernel, publish both kernels together
with `python3 scripts/kernel-release.py publish` before reporting the iteration
complete. If the TCC kernel was edited inside LixinOS, save the disk and use
`publish --tcc-source-disk PATH` to include its guest sources. Check and report the
resulting local dated tag. Release tags are permanent. Use a new tag for corrections.
See [the release workflow](docs/kernel-releases.md) for restore and boot commands.
