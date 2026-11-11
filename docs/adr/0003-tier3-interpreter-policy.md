# Tier 3 is a permanent floor; drive its coverage to zero in practice

**Status:** accepted (2026-10-16)

The adopted runner dispatches every guest PC in tiers: recompiled banks
(**Tier 1**), a dirty-RAM JIT that is designed but not implemented in `v0.6.10`
(**Tier 2**), and a bounded interpreter that executes the guest's own bytes
where no bank covers a PC (**Tier 3**). Tier 3 cannot be removed — code the
recompiler cannot see ahead of time (RAM-resident copies, swapped overlays) is
exactly what it exists for. So the project requirement is not "no interpreter"
but "no interpreter *in practice*": Tier 3 stays as the always-correct floor,
and we bank everything the recompiler can see (ARM9, ARM7 at its runtime
addresses, overlays ahead of time) so that Tier 3 serves **< 0.1 %** of executed
guest instructions during menus and overworld, read from the runner's coverage
manifest at every milestone.

Considered:

- Chasing literal zero interpretation — rejected: impossible for genuinely
  RAM-mutated code, and enforcing it would mean not running the game's own bytes
  where the game mutates them.
- The runner's live-overlay path, which recompiles guest bytes with a host C
  compiler at run time and `dlopen`s the result — rejected for the shipped
  configuration: it puts a compiler in the runtime, which is the thing this
  project is not building.

Consequence: the interpreter's share of execution is a **tracked metric**, and
"the game is slow / doesn't progress" is first diagnosed as a coverage question,
not a performance one.

**Verification (2026-11-11).** First measurement on a *gameplay* window rather
than a boot window — Black, save-loaded, 262 emulated seconds, driven to the
overworld: **0.020 %** of executed guest instructions (ARM9 0.011 %, ARM7
0.039 %) — 5.35 M interpreted of 26.3 G guest cycles, 0.164 ms of a 13.1 ms
frame. Before the overlay and ARM7 game-play banks it was 0.263 % (0.164 vs
1.78 ms/frame). Rows and the digest control: `re/notes/recompilation.md` §"In
play, the interpreter is 0.02% of guest cycles".
