`assets.exe` is a backup tool for your art assets.

```
assets push      # send your changes
assets pull      # get everyone else's
```
# Tutorial

1. Type in your Windows console
```
winget install assets
```
2. Navigate to the directory that contains your assets. 
```
cd \GameProject
notepad assets.txt
```
3. Write `assets.txt` providing remote location where the assets should live and relative path where the assets are in now.
```
remote user@nas:stores/game
folder art
folder audio
ignore *.tmp
```

# Usage
You can now call `assets push` or `pull` to sync your art asset changes to the remote storage.
Files are deduplicated in chunks, so only changed data is transferred.
Two artists editing `grunt_model.fbx` at the same time results in two variants.
`assets` is meant for large binary files (art, audio, databases), alongside or instead of git, but it can hold anything, including code.




# Requirements

- Windows 10 or later. ssh stores use Windows' OpenSSH client.
- ssh servers need bash, tar and GNU coreutils.
- Paths up to about 190 characters. Longer paths are reported.
- Timestamps, empty folders and symlinks aren't preserved.

# Reference

## Commands

Run from the folder that holds `assets.txt`, or any folder below it.

```
assets status [PATH...]        list what differs between this checkout and the remote; changes nothing
assets pull [PATH...]          bring the remote's changes here
assets push [PATH...]          pull, then send this checkout's changes
assets log [PATH...]           list the pushes of the last 30 days, newest first
assets restore PATH VERSION    put back a version of a file from the log
assets verify                  check every chunk and file on the remote against its hash
assets version                 print the version and the store format
assets help                    print the usage
```

A PATH is a file or folder, relative to the current folder, inside a synced folder (or holding one). With
PATHs, a command acts only on them and leaves everything else as it is: `assets push art/level1` sends
`level1` and keeps other changes here.

Exit code 0 on success, 1 on any failure. `status` exits with 2 when the remote has changes to pull, so a
build script can warn about stale files.

## assets.txt

One setting per line. `#` starts a comment line.

```
remote origin            git's origin remote with /assets appended (w.git -> w.git/assets)
remote HOST:DIR          a folder on an ssh server, created on first push
remote D:\store          a local folder, mapped drive or \\server\share\folder
folder art               a folder to sync, inside the repository; one line each
ignore *.lock            file or folder names to leave out; * and ? match any characters
```

Exactly one `remote`, at least one `folder`. Folders can't contain `..` or overlap. Names match case-insensitively.
`.git`, `.assets` and `assets.txt` itself never sync. Commit `assets.txt` with the project: every checkout
shares it.

## Output

```
pull      A    2.9 MB  art/rock.png    added on the remote
pull      M   68.4 KB  art/tree.png    changed on the remote
pull      D       3 B  art/note.txt    deleted on the remote
push      A|M|D ...                    the same, here
kept        2.9 MB  art/rock.png       deleted on the remote, changed here: push adds it back
restored    2.9 MB  art/rock.png       deleted here, changed on the remote: pull brings it back
conflict    2.9 MB  art/rock.png       changed on both sides
to pull: 2 files, 3.0 MB; 1 deleted here
to push: 1 file, 3.4 MB; 0 deleted on the remote
pushed: 1 file changed, 34 chunks sent (568.4 KB), 0 files deleted (kept on the remote for 30 days)
```

Sizes are whole files. A transfer sends only the chunks the other side lacks; `pushed:` and `pulled:` give the
bytes that travelled.

## Conflicts

A file changed here and on the remote since the last sync is kept twice. Pull renames this checkout's copy to
`NAME.conflict-CHECKOUT-TIME.EXT` and brings the remote's version. Push sends both. Neither command stops.
Keep one copy and delete the other, or combine them by hand, then push. assets never picks one. Until then
`status`, `pull` and `push` list the copies under `needs your attention`.

## History

The remote keeps one current version of every file. Replaced and deleted files stay on the remote for 30 days,
then their chunks are deleted. There are no branches: every checkout of a store shares one history. A branch
that needs different files uses new names, or its own store (a different `remote`).

`assets log` lists each push in the history, newest first, with the files it added (A), changed (M) or
deleted (D). The 8 hex digits before a path name that version of the file:

```
2026-10-28 14:03:12
    M    2.9 MB  b24f19a4  art/rock.png
    D   68.4 KB  0b16be87  art/tree.png
```

`assets restore art/tree.png 0b16be87` writes that version to `art/tree.png`. It is then a change here like
any other: `status` lists it, `push` sends it as the newest version. Restore refuses to replace a file whose
contents the remote doesn't have; push or move it first.

## git

`assets` refuses a remote that is a git repository, inside a git work tree, or inside this repository. If git
tracks a synced folder, or doesn't ignore it, `assets` warns and asks:
`[y]es, [n]o, [a]dd the folders to .gitignore and continue`. The per-checkout state in `.assets/` ignores itself.

## Store layout

```
format            "assets 1"
manifest.txt      one line per file: hash size path, sorted by path
manifests/        earlier manifests, deleted after 30 days
files/HASH        a file: the list of its chunks
chunks/HASH       4 KB to 64 KB of a file, named by its SHA-256
incoming/         pushes in progress
```

A push uploads into `incoming/`, checks every hash, then replaces `manifest.txt` only if no other push replaced
it first. Don't edit a store by hand.
