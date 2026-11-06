# Steps 1–6 repair status

Starting revision: `3bd2e68d56342a11349a382db02e485742859581`.
Repair branch: `repair/steps-1-6-audit`.

| Area | Status | Evidence / limitation |
|---|---|---|
| Step 1.1–1.3 architecture, threat model, interfaces | Implemented | [ARCHITECTURE.md](./ARCHITECTURE.md), [THREAT_MODEL.md](./THREAT_MODEL.md), [SPECULATION_DESIGN.md](./SPECULATION_DESIGN.md) |
| Step 1.4 baseline | Partial | Baseline artifacts exist; repeated statistical benchmarking remains later work. |
| Step 1.5 instrumentation journal | Partial | Git history and counters exist; a complete structured event stream remains future work. |
| Step 2 syscall and lifecycle | Implemented in source | [spec.c](./xv6-public/spec.c), [xspec.c](./xv6-public/xspec.c); current QEMU `xspec` passed. |
| Step 3 shadow storage and quotas | Implemented in source | [shadow.c](./xv6-public/shadow.c), [shadow.h](./xv6-public/shadow.h). |
| Step 4 filesystem COW | Implemented | Filesystem image wrappers are used instead of fake shadow `struct buf` objects. |
| Step 5 namespace isolation | Implemented in source | Create, overwrite, unlink and reservation paths are covered by `xspec`; consistency stress remains limited. |
| Step 6 commit/abort/journal | Implemented in source | Journal admission ticket, publication gate, base-generation validation and ordered output are in [spec.c](./xv6-public/spec.c), [log.c](./xv6-public/log.c), and [fs.c](./xv6-public/fs.c). |
| Step 6.5 crash matrix | Runtime-unverified | No crash-injection harness is claimed by this repair. |

The gate is deliberately coarse at publication and block/cache admission. It
was tested against the project guest paths; full operation-level reader/writer
gate instrumentation and later scalability work remain separate tasks.
