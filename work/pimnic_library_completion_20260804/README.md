# PIMNIC library work tree

This directory is an isolated implementation of
`docs/pimnic-library-plan.md` and `docs/pimnic-app-transfer-api.md`.
It does not modify any source under `benchmarks/`.

The implementation is split into:

- `include/pimnic/abi`: shared, versioned wire and ring ABI.
- `include/pimnic/host` and `host`: host deployment/control API.
- `include/pimnic/dpu` and `dpu`: PE ring API and persistent-kernel state.
- `paradigm`: PE WQE/CQE wrappers and host topology/preload API.
- `apps`: new application adapters and software golden models.
- `tests`: portable ABI/host tests plus a hardware-compatible PE echo.

Run `./build.sh` on PIM1. The script creates only `build/` below this
directory.

See `IMPLEMENTATION.md` for the interface-to-file map and
`VALIDATION.md` for the executed validation record.
