#!/bin/sh
# assets regression (build first: build.bat). Two checkouts, A and B, each an assets repository with folders
# art and db, sync through a store: a local folder (default, offline), or a scratch folder on a server
# (./run.sh ssh HOST: a folder assets-test/run-<pid> in HOST's home, deleted at the end). Everything happens
# in a temp folder outside any git work tree. Every check prints ok or FAILED (then the last command's
# output); the exit code is the number of failures. Needs Git Bash, git and PowerShell.
#   ./run.sh                     against a folder store
#   ./run.sh ssh user@host       against a server (adds the race between two pushes, which needs a slow upload)
cd "$(dirname "$0")" || exit 1
Exe="$(pwd)/../build/assets.exe"
[ -x "$Exe" ] || { echo "build/assets.exe is missing: run build.bat first"; exit 1; }
Work=$(mktemp -d)
Mode=${1:-local}
if [ "$Mode" = ssh ]; then
    Host=${2:?usage: run.sh ssh user@host}
    Scratch=assets-test/run-$$
    ssh -o BatchMode=yes -o ConnectTimeout=10 $Host true 2>/dev/null || { echo "can't reach $Host"; exit 1; }
    trap 'ssh $Host "rm -rf $Scratch; rmdir assets-test 2>/dev/null"; rm -rf "$Work"' EXIT
    Remote=$Host:$Scratch/store
    server() { ssh $Host "cd $Scratch/store && $1"; }
else
    trap 'rm -rf "$Work"' EXIT
    mkdir -p "$Work/store"
    Remote=$(cygpath -w "$Work/store")
    server() { (cd "$Work/store" && eval "$1"); }
fi
Failed=0
Log="$Work/last.txt"

# run CHECKOUT VERB: assets from CHECKOUT's art folder (it finds assets.txt above); the output goes to $Log
run() { (cd "$Work/$1/art" 2>/dev/null || cd "$Work/$1"; timeout 300 "$Exe" "$2") > "$Log" 2>&1 < /dev/null; }
A() { run A "$1"; }
B() { run B "$1"; }
check() {
    if eval "$2"; then echo "ok      $1"; else echo "FAILED  $1"; sed 's/^/        /' "$Log"; Failed=$((Failed + 1)); fi
}
# the synced files of a checkout, with their hashes
tree() { (cd "$Work/$1" && find art db -type f ! -name "*.lock" 2>/dev/null | LC_ALL=C sort | while read -r f; do echo "$(sha256sum < "$f" | cut -c1-64) $f"; done); }
# "pushed: F files changed, C chunks sent (K KB)" / "pulled: C chunks fetched (K KB), R reused"
sent() { sed -n 's/^pushed: .* \([0-9]*\) chunks sent.*/\1/p' "$Log"; }
fetched() { sed -n 's/^pulled: \([0-9]*\) chunks fetched.*/\1/p' "$Log"; }
reused() { sed -n 's/^pulled: .*, \([0-9]*\) reused.*/\1/p' "$Log"; }
for C in A B; do
    mkdir -p "$Work/$C"
    printf 'remote %s\nfolder art\nfolder db\nignore *.lock\n' "$Remote" > "$Work/$C/assets.txt"
done

# --- assets.txt: found from below, and its rules ---
mkdir -p "$Work/none" "$Work/C"
check "no assets.txt: not a repository" '! run none status && grep -q "not an assets repository" "$Log"'
printf 'remote %s\nfolder ../outside\n' "$Remote" > "$Work/C/assets.txt"
check "folder with .. refused" '! run C status && grep -q "inside the repository" "$Log"'
printf 'remote %s\nfolder art\nfolder art/sub\n' "$Remote" > "$Work/C/assets.txt"
check "overlapping folders refused" '! run C status && grep -q "overlap" "$Log"'
printf 'remote %s\nfolder art\ncolour blue\n' "$Remote" > "$Work/C/assets.txt"
check "unknown setting refused" '! run C status && grep -q "unknown setting" "$Log"'
printf 'folder art\n' > "$Work/C/assets.txt"
check "no remote refused" '! run C status && grep -q "needs a remote" "$Log"'

# --- first push and pull: spaces, non-ASCII, no extension, an empty file, a file of many chunks ---
mkdir -p "$Work/A/art/café art" "$Work/A/db"
printf red > "$Work/A/art/rock.png"
printf one > "$Work/A/art/café art/rock 1.png"
printf readme > "$Work/A/art/README"
: > "$Work/A/art/empty.txt"
printf locked > "$Work/A/art/project.lock"
printf outside > "$Work/A/notsynced.txt"
head -c 3000000 /dev/urandom > "$Work/A/db/big.bin"
check "first push, run from a subfolder" 'A push && grep -q "^pushed: 5 files changed" "$Log"'
check "ignored and outside files stay out" '! server "grep -qE \"project.lock|notsynced\" manifest.txt"'
check "status after push: in sync" 'A status && grep -q "^5 files the same" "$Log" && ! grep -qE "^(pull|push) " "$Log"'
check "pull into an empty checkout" 'B pull && [ "$(tree A)" = "$(tree B)" ]'
check "version" '"$Exe" version > "$Log" && grep -q "store format" "$Log"'
check "the store has a format" 'server "grep -qx \"assets 1\" format"'
check ".assets/ hides itself from git" '[ "$(cat "$Work/A/.assets/.gitignore")" = "*" ]'
check "verify" 'A verify && grep -q "intact" "$Log"'

# --- deltas: a small edit, and an insert that shifts every byte after it, send about one chunk ---
printf 'EDITED' | dd of="$Work/A/db/big.bin" bs=1 seek=1500000 conv=notrunc 2>/dev/null
check "small edit sends 1-3 chunks" 'A push && [ "$(sent)" -ge 1 ] && [ "$(sent)" -le 3 ]'
check "pull of the edit reuses the rest" 'B pull && [ "$(fetched)" -le 3 ] && [ "$(reused)" -ge 100 ] && [ "$(tree A)" = "$(tree B)" ]'
{ printf 'inserted at the start'; cat "$Work/A/db/big.bin"; } > "$Work/A/big.tmp" && mv "$Work/A/big.tmp" "$Work/A/db/big.bin"
check "insert at the start sends 1-3 chunks" 'A push && [ "$(sent)" -le 3 ]'
check "pull of the insert reuses the rest" 'B pull && [ "$(fetched)" -le 3 ] && [ "$(tree A)" = "$(tree B)" ]'

# --- renames move no chunks either way, and the emptied folder goes ---
mkdir -p "$Work/A/art/ui" && mv "$Work/A/art/café art" "$Work/A/art/ui/café art" && mv "$Work/A/db/big.bin" "$Work/A/db/moved.bin"
check "rename sends nothing" 'A push && [ "$(sent)" = 0 ]'
check "pull of the rename fetches nothing" 'B pull && [ "$(fetched)" = 0 ] && [ ! -e "$Work/B/art/café art" ] && [ "$(tree A)" = "$(tree B)" ]'

# --- a remote entry outside a checkout's folders is left alone ---
printf 'remote %s\nfolder art\nignore *.lock\n' "$Remote" > "$Work/B/assets.txt"
printf b-only > "$Work/B/art/b.png"
check "push without the db folder keeps db on the remote" 'B push && server "grep -q db/moved.bin manifest.txt"'
printf 'remote %s\nfolder art\nfolder db\nignore *.lock\n' "$Remote" > "$Work/B/assets.txt"
A pull > /dev/null

# --- conflicts keep both copies ---
printf A2 > "$Work/A/art/rock.png"
printf B2 > "$Work/B/art/rock.png"
A push > /dev/null
check "conflict: the remote's at its path" 'B push && [ "$(cat "$Work/B/art/rock.png")" = A2 ]'
check "conflict: this checkout's renamed" '[ "$(cat "$Work"/B/art/rock.conflict-B-*.png)" = B2 ] && grep -q "needs your attention" "$Log"'
check "conflict copy reaches the other checkout" 'A pull && ls "$Work"/A/art/rock.conflict-B-*.png > /dev/null'
printf RA > "$Work/A/art/README"
printf RB > "$Work/B/art/README"
A push > /dev/null
check "conflict on a file with no extension" 'B push && [ "$(cat "$Work/B/art/README")" = RA ] && [ "$(cat "$Work"/B/art/README.conflict-B-*)" = RB ]'
rm "$Work"/B/art/rock.conflict-B-* "$Work"/B/art/README.conflict-B-*
check "resolving: delete the copy, push, pull" 'B push && A pull && ! ls "$Work"/A/art/*.conflict-* 2> /dev/null && [ "$(tree A)" = "$(tree B)" ]'

# --- deleted on one side, changed on the other: the changed copy stays ---
rm "$Work/A/art/rock.png"
A push > /dev/null
printf kept > "$Work/B/art/rock.png"
check "kept: changed here, deleted on the remote" 'B push && grep -q "^kept" "$Log" && A pull && [ "$(cat "$Work/A/art/rock.png")" = kept ]'
printf restored > "$Work/B/art/rock.png"
B push > /dev/null
rm "$Work/A/art/rock.png"
check "restored: deleted here, changed on the remote" 'A pull && grep -q "^restored" "$Log" && [ "$(cat "$Work/A/art/rock.png")" = restored ]'

# --- without the base: the same files stay in sync, different ones keep both ---
check "one base per remote" '[ $(ls "$Work"/B/.assets/base-*.txt | wc -l) = 1 ] && [ ! -e "$Work/B/.assets/base.txt" ]'
rm "$Work"/B/.assets/base-*.txt
check "lost base: same files in sync" 'B status && ! grep -qE "^(conflict|pull|push) " "$Log"'
printf lostbase > "$Work/B/art/README"
check "lost base: a difference keeps both" 'B push && [ "$(cat "$Work"/B/art/README.conflict-B-*)" = lostbase ]'
rm "$Work"/B/art/README.conflict-B-*
B push > /dev/null
A pull > /dev/null

# --- a chunk missing on the remote: the pull fails cleanly, push refuses, then both recover ---
head -c 200000 /dev/urandom > "$Work/A/db/new.bin"
A push > /dev/null
Chunk=$(server "sed -n 1p files/\$(grep ' db/new.bin\$' manifest.txt | cut -c1-64)")
server "mv chunks/$Chunk held-chunk"
printf mine > "$Work/B/art/mine.png"
check "missing chunk: pull fails" '! B pull && grep -q "rebuild" "$Log" && [ ! -e "$Work/B/db/new.bin" ]'
check "missing chunk: push refuses" '! B push && grep -q "not pushing" "$Log" && ! server "grep -q mine.png manifest.txt"'
check "verify finds it" '! A verify'
server "mv held-chunk chunks/$Chunk"
check "after repair: push and verify" 'B push && A pull && [ "$(tree A)" = "$(tree B)" ] && A verify'

# --- a file another program holds open can't be read: nothing is pulled or pushed ---
printf locked > "$Work/A/art/held.png"
rm -f "$Work/locked.ready"
powershell -NoProfile -Command "\$f = [IO.File]::Open('$(cygpath -w "$Work/A/art/held.png")', 'Open', 'ReadWrite', 'None'); New-Item '$(cygpath -w "$Work/locked.ready")' | Out-Null; Start-Sleep 15; \$f.Close()" &
while [ ! -e "$Work/locked.ready" ]; do sleep 1; done
check "locked file: refuses" '! A push && grep -q "couldn.t be read" "$Log"'
wait
check "after unlock: pushes" 'A push'

# --- two pushes at once (ssh only: A must still be uploading 100 MB when B commits) ---
if [ "$Mode" = ssh ]; then
    head -c 100000000 /dev/urandom > "$Work/A/db/huge.bin"
    B pull > /dev/null
    (cd "$Work/A" && timeout 300 "$Exe" push) > "$Work/race.txt" 2>&1 < /dev/null &
    sleep 2
    printf race > "$Work/B/art/race.png"
    B push; RaceB=$?
    wait
    check "race: B commits, A is refused" '[ $RaceB = 0 ] && grep -q "changed during this push" "$Work/race.txt"'
    check "race: A's retry lands both" 'A push && B pull && [ -e "$Work/A/art/race.png" ] && [ -e "$Work/B/db/huge.bin" ] && [ "$(tree A)" = "$(tree B)" ]'
    rm "$Work/A/db/huge.bin"
    A push > /dev/null
fi

# --- trash: history older than 30 days goes, with what only it needed ---
head -c 300000 /dev/urandom > "$Work/A/db/old.bin"
A push > /dev/null
rm "$Work/A/db/old.bin"
A push > /dev/null
Before=$(server "ls chunks | wc -l")
check "deleted file kept 30 days (in the history)" 'server "grep -q db/old.bin manifests/*.txt" && A verify'
server "touch -d '40 days ago' manifests/*.txt"
printf trash > "$Work/A/art/trash.png"
check "trash: old history and its chunks go" 'A push && [ "$(server "ls manifests | wc -l")" -eq 1 ] && [ "$(server "ls chunks | wc -l")" -lt "$Before" ] && A verify'

# --- verify catches a damaged chunk ---
Chunk=$(server "ls chunks | head -1")
server "cp chunks/$Chunk kept-chunk && printf x >> chunks/$Chunk"
check "verify: damaged chunk" '! A verify && grep -q "bad chunk" "$Log"'
server "mv kept-chunk chunks/$Chunk"
check "verify: repaired" 'A verify'

# --- another remote (a branch whose assets.txt names its own store): files are new there, never deletions ---
if [ "$Mode" = ssh ]; then Other=$Host:$Scratch/other; else Other=$(cygpath -w "$Work/other"); fi
A pull > /dev/null
Before=$(tree A)
printf 'remote %s\nfolder art\nfolder db\nignore *.lock\n' "$Other" > "$Work/A/assets.txt"
check "another remote: status shows only adds" 'A status && grep -q "^push      A " "$Log" && ! grep -qE "^(pull|conflict|restored|kept)" "$Log"'
check "another remote: push fills it, nothing here changes" 'A push && [ "$(tree A)" = "$Before" ] && A verify'
printf 'remote %s\nfolder art\nfolder db\nignore *.lock\n' "$Remote" > "$Work/A/assets.txt"
check "back to the first remote: in sync" 'A status && ! grep -qE "^(pull|push|conflict|restored|kept)" "$Log" && [ "$(tree A)" = "$Before" ]'

# --- a store path that would leave the repository is refused before anything is written ---
server "cp manifest.txt manifest.keep && echo \"\$(head -c 64 manifest.txt) 1 art/../escaped.txt\" >> manifest.txt"
check "unsafe store path: refused" '! B pull && grep -q "unsafe" "$Log" && [ ! -e "$Work/B/escaped.txt" ] && [ ! -e "$Work/escaped.txt" ]'
server "mv manifest.keep manifest.txt"
check "unsafe store path: fine again once fixed" 'B pull'

# --- a path too long for Windows' MAX_PATH is reported, and nothing is pulled or pushed ---
Long=$(printf 'x%.0s' $(seq 1 200)).png
printf long > "$Work/A/art/$Long" 2>/dev/null || powershell -NoProfile -Command "Set-Content -LiteralPath '\\?\$(cygpath -w "$Work/A/art")\$Long' long"
check "long path: refused with its length" '! A push && grep -q "path too long" "$Log"'
rm -f "$Work/A/art/$Long"
check "long path: fine again once shortened" 'A push'

# --- a store made by a newer version is refused ---
server "echo \"assets 9\" > format"
check "unknown store format refused" '! A status && grep -q "update assets" "$Log"'
server "echo \"assets 1\" > format"

# --- git: a folder git would also keep is warned about, and push asks ---
G="$Work/G"
mkdir -p "$G/art"
git init -q "$G"
printf 'remote %s\nfolder art\n' "$Remote" > "$G/assets.txt"
printf one > "$G/art/a.png"
check "git: status warns about an unignored folder" '(cd "$G" && "$Exe" status) > "$Log" 2>&1 < /dev/null && grep -q "aren.t ignored by git" "$Log" && grep -q "remote copy" "$Log"'
check "git: no console answers no" '! (cd "$G" && "$Exe" push) > "$Log" 2>&1 < /dev/null && grep -q "no answer" "$Log"'
check "git: n stops" '! (cd "$G" && echo n | "$Exe" push) > "$Log" 2>&1 && grep -q "stopped" "$Log"'
check "git: a adds the folder to .gitignore and pushes" '(cd "$G" && echo a | "$Exe" push) > "$Log" 2>&1 && grep -qx "/art/" "$G/.gitignore" && grep -q "^pushed" "$Log"'
check "git: then no warning" '(cd "$G" && "$Exe" status) > "$Log" 2>&1 && ! grep -q "remote copy" "$Log"'
check "git: git sees assets.txt, not the state" 'git -C "$G" status --porcelain -uall > "$Log" && grep -q "assets.txt" "$Log" && ! grep -q "\.assets/" "$Log"'
git -C "$G" add -f art/a.png && git -C "$G" -c user.name=t -c user.email=t@t commit -qm tracked
check "git: a tracked file is warned about" '(cd "$G" && "$Exe" status) > "$Log" 2>&1 && grep -q "git tracks 1" "$Log"'
printf two > "$G/art/a.png"
check "git: y goes on" '(cd "$G" && echo y | "$Exe" push) > "$Log" 2>&1 && grep -q "^pushed" "$Log"'

# --- folder . syncs the whole tree, but never assets.txt, which belongs to the repository ---
mkdir -p "$Work/W/sub"
printf 'remote %s\nfolder .\n' "$(cygpath -w "$Work/wholestore")" > "$Work/W/assets.txt"
printf top > "$Work/W/top.png"
printf sub > "$Work/W/sub/s.png"
check "folder .: syncs the tree, not assets.txt" '(cd "$Work/W" && "$Exe" push) > "$Log" 2>&1 < /dev/null && grep -q "^pushed: 2 files" "$Log" && ! grep -q "assets.txt" "$Work/wholestore/manifest.txt"'

# --- remotes that are git's own space are refused ---
if [ "$Mode" = ssh ]; then
    ssh $Host "git init -q --bare $Scratch/bare.git && git init -q $Scratch/work"
    Bare=$Host:$Scratch/bare.git
    Inside=$Host:$Scratch/work/assets
else
    git init -q --bare "$Work/bare.git"
    git init -q "$Work/work"
    Bare=$(cygpath -w "$Work/bare.git")
    Inside=$(cygpath -w "$Work/work/assets")
fi
mkdir -p "$Work/R/art"
printf 'remote %s\nfolder art\n' "$Bare" > "$Work/R/assets.txt"
check "remote that is a git repository refused" '! run R status && grep -q "git repository itself" "$Log"'
printf 'remote %s\nfolder art\n' "$Inside" > "$Work/R/assets.txt"
check "remote inside a git work tree refused" '! run R status && grep -q "inside a git work tree" "$Log"'
if [ "$Mode" != ssh ]; then
    printf 'remote %s\nfolder art\n' "$(cygpath -w "$Work/R/store")" > "$Work/R/assets.txt"
    check "remote inside the repository refused" '! run R status && grep -q "inside this repository" "$Log"'
fi

# --- remote origin: <origin's repo>.git/assets, also when origin's URL leaves out .git ---
O="$Work/O"
mkdir -p "$O/art"
git init -q "$O"
if [ "$Mode" = ssh ]; then
    ssh $Host "git init -q --bare $Scratch/origin.git"
    git -C "$O" remote add origin "$Host:$Scratch/origin"
    store() { ssh $Host "cd $Scratch/origin.git/assets && $1"; }
else
    git init -q --bare "$Work/origin.git"
    git -C "$O" remote add origin "$(cygpath -w "$Work/origin")"
    store() { (cd "$Work/origin.git/assets" && eval "$1"); }
fi
printf 'remote origin\nfolder art\n' > "$O/assets.txt"
printf '/art/\n' > "$O/.gitignore"
printf origin > "$O/art/o.png"
check "remote origin: pushes into origin.git/assets" '(cd "$O" && "$Exe" push) > "$Log" 2>&1 < /dev/null && store "grep -q art/o.png manifest.txt"'

[ "$Failed" = 0 ] && echo "all passed ($Mode)" || echo "$Failed FAILED ($Mode)"
exit $Failed
