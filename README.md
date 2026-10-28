# assets

**Big files next to git, without the pain.** `assets` keeps your art, sounds, models and databases in one
copy on your own server or NAS, outside git, and sends only what changed.

```
assets push      # send your changes
assets pull      # get everyone else's
```

## Why assets

- **It never blocks you.** When a file changed in two places, you get both: theirs at its path, yours
  beside it as `rock.conflict-<you>-<time>.png`. Pull and push carry on. Resolve collisions when it
  suits you.
- **Only changes travel.** Files are split into content-defined chunks, stored once. A re-save of a
  155 MB database sends 7 MB. Moving or renaming a folder sends nothing.
- **It stays out of git.** No pointer files, hooks or special server. Git sees one small config file.
- **Your storage.** Any Linux box you can ssh into, or a plain folder: local, mapped drive or network
  share. No service, no account.
- **One small program.** A single `.exe` with no dependencies, built from one C++ file.
- **Safe.** A push that stops halfway changes nothing, every chunk is checked against its hash, and
  deleted files stay recoverable for 30 days.

## Compared to Git LFS and git-annex

| | assets | Git LFS | git-annex |
|---|---|---|---|
| Conflicts | **both kept, never blocks** | git merge conflict | both kept as variants |
| Edit to a 1 GB file sends | **only changed chunks** | whole file | whole file |
| In git | **one config file** | pointer per asset | symlink per asset |
| Server | **ssh + bash, or a folder** | LFS server | ssh + git-annex |
| Asset history per commit | no (30-day trash) | yes | yes |
| Fetch only some files | no | partly | yes |
| Platforms | Windows | all | all |

Pick **assets** to keep asset folders in sync with the least ceremony, especially huge files that change
a little at a time. Pick LFS or git-annex if you need every old version tied to its commit.

## Setup

Build from a Visual Studio x64 developer prompt, then put `build\assets.exe` on your PATH:

```
build.bat
```

Add `.assets\config` at your repository's root and commit it:

```
remote origin
folder art
folder sounds
ignore *.tmp
```

| Line | Meaning |
|---|---|
| `remote origin` | Store in your git origin's repository, under `/assets` (needs an ssh or folder origin). |
| `remote HOST:DIR` | Store over ssh, like `git@nas:backups/game`. |
| `remote C:\folder` | Store in a folder, mapped drive or share (`\\nas\backups\game`). |
| `folder PATH` | A folder to sync, relative to the repository root. One line each. |
| `ignore NAME` | File or folder names to skip. `*` and `?` are wildcards. |

The store is created on first push. GitHub and similar hosts can't hold it, so point `remote` at your
own server or a folder.

## Use

From anywhere in the repository:

```
assets status    what changed here, in the store, or both
assets pull      bring the store's changes here
assets push      pull, then send your changes
assets verify    check every chunk and rebuild every stored file
```

Collisions are listed by every command until you keep one copy and delete the other. A file another
program has locked stops pull and push, rather than looking deleted.

If git also tracks or would add files in an asset folder, `assets` warns you (two remote copies can
disagree) and asks whether to continue, stop, or add the folder to `.gitignore`.

## Branches

The store has no branches; every checkout gets its current state. Two ways to work with that:

- **Let assets only grow.** Branch your code, not your art. To change an asset another branch still
  uses, add a new file (`rock_v2.png`) instead of editing it.
- **Give a branch its own store.** Its `.assets\config` names another remote, and it gets its own
  checkout (`git worktree add`), since `git checkout` doesn't swap untracked asset folders. Switching
  stores never deletes files.

## Requirements

- Windows 10 or later. ssh stores use Windows' built-in OpenSSH.
- ssh servers need bash, tar and GNU coreutils (any Linux).
- Paths up to about 190 characters. Timestamps, empty folders and symlinks aren't kept.

Tests: `tests\run.sh` (offline) and `tests\run.sh ssh user@host`, in Git Bash.

## License

MIT
