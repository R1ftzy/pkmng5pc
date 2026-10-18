# Bring-up fixes are declared, address-level bank patches

**Status:** accepted (2026-10-18)

Bring-up will keep hitting places where the game's own code reaches a fatal path
that is correct for a shipped console but wrong for our environment. The first
was the NEW GAME transition's sync facade releasing resource `0x43` twice: the
second release fails the facade's own `lock_leave_raw` assert
(`0x02075F7C`, `bl 0x02088234`) and calls the game's fatal-halt routine, which
parked the ARM9 at `0x02088234` forever.

We fix such places in **our tooling, at addresses** — never by editing generated
code and never by touching `third_party/ndsrecomp`. A per-title config
(`re/config/<title>.toml`) may declare `[[patch]]` entries:

```toml
[[patch]]
address = 0x02075F7C
kind    = "skip_call"
reason  = "lock_leave_raw id-mismatch assert: leaving an idle lock is a no-op"
```

`kind` is one of `skip_call` (replace a `bl`/`blx` with a PC-advance
fall-through) or `stub_return` (empty a function to a return). `pkmn_bankgen`
applies a patch to the emitted shard text while it writes the bank, prints one
line per application, and **fails loudly** when a declared address is missing,
is not the instruction kind the patch claims, or the kind is unknown.

Every patch is therefore auditable (address + kind + reason in a tracked file),
reversible (delete the block, regenerate the bank), title-scoped, and free of
ROM content. `banks/` stays a regenerable artifact.

Considered:

- Editing generated shard C by hand — rejected: `banks/` is regenerated, so hand
  edits are silently lost and unreviewable.
- Patching the vendored submodule — rejected: it stays pristine at `v0.6.10` so
  it can be re-pulled; workarounds live in our tree.
- HLE-style stubs of whole subsystems — rejected as the default: the project's
  stance is faithful LLE execution (`Plan.md` M2.2). A patch is a **point**
  assertion bypass, not a reimplementation of behavior.
- Making all asserts non-fatal, i.e. stubbing the halt routine `0x02088234`
  globally — rejected: 54 call sites would silently pass and hide real faults;
  the patch is scoped to the one branch bring-up proved wrong for us.

Consequence: each patch carries a `reason` citing the observed behavior it
compensates for; the list is small and lives in `re/config/`; and "the game
halted" is still diagnosed from evidence first — a patch is only declared after
a probe names the failing assert. These patches are **bring-up scaffolding and a
game-logic seam**: `AGENTS.md` §8 ("do not refactor decompiled code") is
respected because no recompiled unit is edited, and the broader policy for
game-logic interception (M11 hooks) remains separate and is still owed an ADR.
