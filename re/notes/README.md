# Reverse-engineering notes

Per-subsystem findings for the Gen 5 ROMs. Every claim here must be
evidence-backed (see `AGENTS.md` section 6).

Files to maintain (create as work begins):

- `header.md` — NDS header, game codes, per-ROM SHA-256
- `nitrofs.md` — FNT/FAT tree, file IDs, NARC inventory
- `overlays.md` — ARM9/ARM7 overlay table, compression, load addresses
- `arm9-init.md` — entry point, init sequence, boot flow
- `recompilation.md` — static-recompilation tooling choice (D7), licenses
- `map-system.md` — matrix/header/area/event data
- `script-vm.md` — SCRCMD bytecode and interpreter
- `text-system.md` — message archives and font
- `pokemon-data.md` — species, moves, items, trainers, encounters
- `battle.md` — battle flow and formulas
- `save.md` — save block layout and checksums
- `3d-ge.md` — NSBMD/NSBTX and GE behavior
- `wide-output.md` — wide top-band output: captures, true-widening measurement
- `sdat-audio.md` — SDAT/SSEQ/SBNK/SWAR/SWAV/STRM

Use `TEMPLATE.md` as the starting point for a new note.

Rules: separate **observations** from **inferences** from **unknowns**. Cite
Evidence IDs and tool versions. Never infer behavior from a filename or string.
