# assets

```
assets push      # send your changes
assets pull      # get everyone else's
```

assets keeps a set of folders in one store on a server or network folder and only transfers changed data.
It doesn't need git. It's meant for large binary files (art, audio, databases), alongside or instead of
git, but it can hold anything, including code that only ever grows.

I wrote it to sync the art for my game and some multi-GB Ghidra databases between machines. Git LFS
uploads the whole file on every change and needs an LFS server.

### Usage

```
assets status    differences between this checkout and the store
assets pull      apply the store's changes
assets push      pull, then upload local changes
assets verify    check every chunk and rebuild every file in the store
```

Any folder with `.assets/config` is a repository. Run from anywhere inside it: assets looks upward for
`.assets/config`, like git does for `.git`.

### Configuration

`.assets/config`:

```
remote git@nas:stores/game
folder art
folder audio
ignore *.tmp
```

`remote` is the store: `HOST:DIR` for ssh, or a local folder, mapped drive or share (`\\nas\backup\game`).
In a git repository, `remote origin` puts it in the origin's repository at `<repo>.git/assets` (git ignores
folders it doesn't know about). GitHub can't host a store. The store is created on the first push.

`folder` is a folder to sync, relative to the repository root (`folder .` for all of it). `ignore` is a
file or folder name to skip (`*` and `?` wildcards). The rest of `.assets/` is local state. If you use
git, commit `.assets/config`; `.assets/.gitignore` hides the rest.

### How it works

Files are split into content-defined chunks (FastCDC, 16 KB average), each stored once by its SHA-256.
Re-saving a 155 MB Ghidra database uploads about 7 MB. Moving or renaming files uploads nothing. A pull
rebuilds files from local chunks plus the missing ones.

A push stages everything, checks each chunk against its hash, then replaces the manifest. An interrupted
push leaves the store unchanged. If another push finished first, it is rejected and has to be run again.
Old manifests are kept for 30 days, so deleted files can be recovered within that time.

Each checkout keeps the manifest of its last sync, so changes are compared three ways. If a file changed
in two checkouts, pull keeps both: the store's version at the original path and the local one renamed to
`name.conflict-<checkout>-<time>.ext`. Pull and push never stop for conflicts. They are listed until one
copy of each pair is deleted.

### History and branches

A store has one current state and no branches. Every checkout pulls the same files, and the only history
is the 30 days of old manifests. This works best for files that only grow: a new version gets a new name
(`rock_v2.png`) instead of replacing one that something else still uses.

### With git

The synced folders should be in `.gitignore`. If git tracks or would add files in them, assets warns and
asks before pulling or pushing (continue, stop, or add the folders to `.gitignore`). Git branches don't
switch the synced folders. A branch that needs different assets can point `remote` at its own store, in a
checkout of its own (`git worktree add`).

### Compared to Git LFS and git-annex

| | assets | Git LFS | git-annex |
|---|---|---|---|
| Edit to a 1 GB file uploads | changed chunks | whole file | whole file |
| Files in git | `.assets/config` | a pointer per file | a symlink per file |
| Server | ssh with bash, or a folder | LFS server | ssh with git-annex |
| Conflicts | both copies kept | merge conflict | both copies kept |
| History per commit | no (30 days) | yes | yes |
| Platforms | Windows | all | all |

### Building

Run `build.bat` from a Visual Studio x64 developer prompt. It produces `build\assets.exe`, with no
dependencies. Tests: `tests\run.sh` (local folder store) and `tests\run.sh ssh user@host` (Git Bash).

### Limitations

- Windows 10 or later. ssh stores use Windows' OpenSSH client.
- ssh servers need bash, tar and GNU coreutils.
- Paths up to about 190 characters. Longer paths are reported.
- Timestamps, empty folders and symlinks aren't preserved.

### License

MIT
