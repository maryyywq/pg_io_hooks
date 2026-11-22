## Summary

<!-- What the change does and why. Link the issue: "Closes #N". -->

## Checklist

- [ ] `scripts/run-tests.sh` passes for each affected PostgreSQL version
      (`PG_VERSION=17|18|19`)
- [ ] `scripts/check-style.sh` passes
- [ ] Patches changed: regenerated with `git format-patch --base=...` for
      every version in `patches/`, and the hooks API stays the same on all
      of them (or `SMGR_HOOKS_API_VERSION` is increased)
- [ ] Documentation (`README.md`, `docs/`) updated
