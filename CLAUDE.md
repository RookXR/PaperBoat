# PaperBoat — Quest VR fork

This checkout is RookXR's fork of HarbourMasters' PaperBoat (Paper Mario port), used
to bring the game to Meta Quest 3 in stereoscopic 3D.

**Before doing anything, read [`docs/quest_vr/README.md`](docs/quest_vr/README.md)**:
status, repo/branch layout, and links to the build/deploy guide, decisions, known
issues, open tasks and the session log.

At the end of a session (or when the owner asks), update `docs/quest_vr/`:
add a `session_log.md` entry, update the status table in `README.md`,
`open_tasks.md`, and record new decisions and known issues.

Work on branch `quest-xr`. Push PaperBoat to the `rookxr` remote and libultraship
(`external/libultraship`) to its `origin` (the RookXR fork); `origin` of this repo is
upstream HarbourMasters and is not pushed to.

**Before every push, check that nothing personally identifying is committed** — in
docs, code comments, or commit messages: local file paths (user name, drive
letters), LAN IPs, ROM locations, private email addresses. Use placeholders such
as `<repo>` and `<headset-ip>`. A local `pre-push` hook enforces it; see
[`docs/quest_vr/README.md`](docs/quest_vr/README.md) → "Before pushing". Never write
the hook's patterns into tracked files.
