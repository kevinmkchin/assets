# assets

**Big files next to git, without the pain.** `assets` keeps your art, sounds, models and databases in one
copy on your own server or NAS, outside git, and sends only what changed.

```
assets push      # send your changes
assets pull      # get everyone else's
```

## Why assets

**It never blocks you.** When the same file changed in two places, there is no merge, no lock and no
error. You get both: theirs at its path, yours next to it as `rock.conflict-<you>-<time>.png`. Pull and
push carry on, and you resolve collisions when it suits you: tomorrow, next week, or never. Every
command lists the copies waiting for you.

**Only changes travel.** Files are cut into content-defined chunks, stored once each. A re-save of a 155 MB
database sends 7 MB instead of 155, an edit in the middle of a 1 GB file sends a few chunks, and moving,
renaming or copying a whole folder sends nothing at all, in either direction. Every version you keep
costs only its new chunks.

**It stays out of git.** No pointer files, no hooks, no smudge filters, no special server. Git never sees
your assets; it sees one small config file. Clone, branch and rebase the code as you always do.

**Your storage, nothing else.** The store is a plain folder: on any Linux box you can ssh into (your NAS,
a home server, the machine that hosts your git repos), or on a mapped drive or network share. There's no
service to run and no account to create.

**One small program.** A single `.exe`, no runtime, no installer, no dependencies. Built from one C++ file.

**Safe by default.** A push that stops halfway changes nothing. Every chunk is checked against its hash.
Deleted and replaced files stay in the store for 30 days. `assets verify` rebuilds every stored file
to prove the store is whole. Nothing from the store can write outside your repository.

## How it compares

| | assets | Git LFS | git-annex |
|---|---|---|---|
| Conflicts | **both copies kept, pull and push never stop** | a git merge conflict to resolve first (or file locks) | both kept as variant files by `git annex sync` |
| Edit to a 1 GB file sends | **the changed chunks (MBs)** | the whole file | the whole file (by default) |
| Each new version of an edited file stores | **only its new chunks** | the whole file | the whole file |
| Moving or renaming files sends | nothing | nothing | nothing |
| In git | **one config file** | a pointer file per asset | a symlink or pointer per asset |
| Needs on the server | **ssh + bash, or just a folder** | an LFS server | ssh + git-annex, or a special remote |
| Learning curve | **minutes** | low | steep |
| Asset history follows git commits and branches | no: one current state, 30-day trash | yes | yes |
| Fetch only some files | no: whole folders | partly | yes |
| Several stores, redundancy, encryption | no | no | yes |
| Platforms | Windows | all | all (weakest on Windows) |

Pick **assets** when your team wants its asset folders in sync with as little ceremony as possible,
and works with huge binaries that change a little at a time. Pick Git LFS or git-annex when you need
every old version of every asset tied to the commit that used it.

## Setup

Build it from an x64 Visual Studio developer prompt, and put `build\assets.exe` on your PATH:

```
build.bat
```

Then make `.assets\config` at the root of your repository and commit it:

```
remote origin
folder art
folder sounds
ignore *.tmp
```

| Line | Meaning |
|---|---|
| `remote origin` | The store is your git origin's repository plus `/assets` (`git@host:game.git/assets`), next to git's own data. |
| `remote HOST:DIR` | A store over ssh, like `git@nas:backups/game-assets`. For a non-standard port, use a host alias in `~/.ssh/config`. |
| `remote C:\folder` | A store in a folder: local, a mapped drive, or a share (`\\nas\backups\game`). |
| `folder PATH` | A folder to sync, relative to the repository root. Repeat it for more. No `..`, no overlaps. |
| `ignore NAME` | A file or folder name to leave out anywhere in the folders. `*` and `?` are wildcards. |
| `# ...` | A comment. |

The store's folder doesn't have to exist: the first push creates it. `remote origin` needs an ssh or
folder origin; hosted git services like GitHub can't hold the store, so point `remote` at your own server.

## Use

Run from anywhere inside the repository; `assets` finds `.assets\config` the way git finds `.git`.

```
assets status    what changed here, in the store, or both
assets pull      bring the store's changes here
assets push      pull, then send this checkout's changes
assets verify    check every stored chunk, and rebuild every stored file from its chunks
assets version   the program's version and store format
```

## Rules of the road

- **Collisions wait for you.** A file changed both here and in the store keeps both copies (above). A file
  changed here but deleted elsewhere stays and is pushed back; a file deleted here but changed elsewhere
  comes back on pull. Keep the copy you want, delete the other, push.
- **A locked file stops pull and push.** A file another program holds open would look deleted, so nothing
  moves until it's free.
- **Keep the asset folders out of git.** If git tracks a file in a folder, or would add it, both would keep
  a copy and the two could disagree. `assets` warns you, and before pull and push it asks: `y` to go on,
  `n` to stop, or `a` to add the folders to `.gitignore`.
- **The store stays out of git's way.** A store that is a git repository itself, or inside a git work tree
  or this repository, is refused. `<repo>.git/assets` inside a bare repository is fine: git ignores
  folders it doesn't know. (`git clone --mirror` doesn't copy it; move it by hand with the repository.)
- **`.assets\` is yours to commit, partly.** Its own `.gitignore` shows git only `config`; the rest is this
  checkout's state. Deleting the state is safe: the next sync treats every difference as a collision and
  keeps both copies.

## Branches

The store has no branches: every checkout pulls the same current state, whichever git branch it's on.
Two ways to work with that:

**Let the assets only grow (the usual way).** Code and the files that refer to assets branch in git; the
asset folders hold the union of what every branch uses. To change an asset that another branch still
uses, add the new version as a new file (`rock_v2.png`) and point your branch at it. Edit in place only
what no other branch uses, and delete only what no branch will ever need again (30 days in the trash,
no longer).

**Give a branch its own store (for an experiment that changes many assets).** The config is in git, so a
branch can name a different store:

```
git worktree add ../game-experiment experiment
cd ../game-experiment
# in .assets\config on that branch:  remote git@host:game.git/assets-experiment
assets push       # the first push copies this checkout's files into the new store
git commit -am "experiment: its own asset store"
```

- **Give such a branch a checkout of its own** (`git worktree add`, or another clone). Asset folders
  aren't in git, so `git checkout` doesn't swap them; in a shared checkout the next push would send the
  other branch's files to this store.
- **Switching stores is never destructive.** Each checkout remembers its last sync per store, so a store it
  never synced with sees its files as new ones, never as deletions.
- **The stores share nothing.** Bring experiment assets home by copying them into a checkout of the main
  branch and pushing there. Delete an experiment's store by hand when you're done.

## How it works

Each file is cut into chunks where a rolling hash of its bytes says so (FastCDC, 4 to 64 KB, 16 KB on
average), so an edit moves only the cuts around it. The store holds `chunks/<sha256>`, one chunk list per
distinct file content in `files/`, a `manifest.txt` of `hash size path` lines, and `format`. A push sends
the chunks the store lacks, as one tar stream over ssh, to `incoming/`, where every chunk is checked and
every file rebuilt before the manifest is swapped, last. A pull rebuilds each file from the chunks of your
old copy plus the missing ones. Each checkout keeps the manifest of its last sync, which makes the
compare three-way: a change made only elsewhere is pulled, not pushed back. Old manifests stay 30 days,
and so does everything they need.

## Requirements and limits

- **Client:** Windows 10 or later, with Windows' built-in OpenSSH for ssh stores. Git is optional.
- **ssh server:** bash, tar and GNU coreutils (any Linux). macOS, BSD and BusyBox NAS shells aren't
  supported over ssh; use a mapped drive or share instead.
- Paths up to about 190 characters from the drive letter (Windows' `MAX_PATH`, with room for temp files);
  longer ones are reported, never truncated.
- Not kept: file timestamps (pulled files get the time of the pull), empty folders, symbolic links.
- Tests: `tests\run.sh` (offline, a folder store) and `tests\run.sh ssh user@host`. They need Git Bash,
  git and PowerShell.

## License

MIT. See `LICENSE`.
