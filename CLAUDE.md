# ao-vk (randy-vk)

## Changelog

`CHANGELOG.md` records every change a player would notice: new features, fixes, changed or renamed settings,
new diagnostics. Keep it up to date as work lands.

- **Add the entry in the same commit as the change**, under `## Unreleased`, in the right group:
  `New`, `Changed`, `Fixed`, `Diagnostics`. Create a group if it isn't there yet.
- **Write for players, not developers.** Say what they see and where (zone, window, effect), not the
  function or file that changed. "The PF map's black areas showed the world through them" beats "UI layer
  clear wrote alpha 0". Name new or renamed `RVK_` settings and hotkeys.
- One bullet per change, a sentence or two. Add the GitHub issue as `(#12)` when there is one.
- Purely internal work (refactors, the test harness, profiling tools, port bookkeeping) gets no entry unless
  players notice it.
- On a feature branch, add the entry there too. When merging into main, keep the entries from both sides
  under `## Unreleased`.
- **At release:** rename `## Unreleased` to `## X.Y.Z - YYYY-MM-DD`, start a fresh empty `## Unreleased`
  above it, and draft the GitHub release notes from that section.
