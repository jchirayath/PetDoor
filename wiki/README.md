# Wiki source

These pages are the source for the
[GitHub wiki](https://github.com/jchirayath/PetDoor/wiki). They live here so
they are versioned, reviewable and diffable alongside the code — a GitHub wiki
is a separate git repository with no pull requests and no history anyone reads.

**Edit here, then publish:**

```bash
./wiki/publish.sh
```

### One-time setup

GitHub does not create the wiki's git repository until the first page is saved
through the web UI. Until then `PetDoor.wiki.git` does not exist and cloning it
fails, so `publish.sh` cannot bootstrap it:

1. Open <https://github.com/jchirayath/PetDoor/wiki>
2. **Create the first page** — any content, it gets replaced
3. Save, then run `./wiki/publish.sh`

### What is here

| Page | Covers |
|---|---|
| `Home.md` | Landing page — what it is, why, where to start |
| `How-it-works.md` | The signal chain and the safety invariants |
| `Parts-and-cost.md` | What to buy, what it costs, the relay caveat |
| `The-beacon.md` | Why not an AirTag; what to use and how to set it |
| `Converting-a-door.md` | Tapping the controller's buttons |
| `The-dashboard.md` | The optional log server and its analytics |
| `ESP32.md` | The board, for people new to microcontrollers |
| `FAQ.md` | The questions that actually get asked |
| `_Sidebar.md`, `_Footer.md` | Wiki navigation chrome |

### House style

The wiki mirrors [petdoor.aspl.net](https://petdoor.aspl.net): an overview for
someone deciding whether to build one. It **links to `docs/` rather than
duplicating it** — the repository is the reference, and two copies of the same
instruction drift apart.

Keep the honest limits in. The FAQ says plainly that this is not a lock and that
the door cannot see into the doorway, and those answers are the reason the page
is trustworthy.
