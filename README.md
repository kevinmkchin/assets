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
