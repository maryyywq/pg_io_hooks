# pg_io_hooks

Storage I/O transformation hooks for PostgreSQL 17 and prototype
extensions built on top of them (diploma project).

## Layout

- `patches/`    - patches against the PostgreSQL 17 core (`git format-patch`)
- `extensions/` - prototype extensions (`pg_page_crypt`, `pg_io_audit`, ...)
- `tests/`      - test scenarios and benchmark scripts
- `docs/`       - notes, analysis, design documents
- `scripts/`    - build and environment helper scripts

## Status

Work in progress.
