/*
    assets: keeps a repository's folders (typically big files: art, sounds, databases) in one store on a
    server or folder, sending only what changed, and never blocking on a conflict. It doesn't need git; next
    to git it stays out of git's way. MIT license.

        assets status|pull|push|verify|version

    It works in the tree of the nearest assets.txt at or above the current folder, the way git finds .git.
    assets.txt is plain text, one setting per line, and belongs with the repository (commit it, if git):

        remote origin                           <git origin's repo>.git/assets, on origin's host or folder
        remote git@example.com:game.git/assets  or HOST:DIR over ssh
        remote \\nas\backup\game                or a folder: local, a mapped drive or a network share
        folder art                              a folder to sync, inside the repository; repeat for more
        ignore *.lock                           a file or folder name to leave out (* and ? are wildcards)
        # a comment

    The store is content-addressed and delta-synced. Every file is cut into content-defined chunks (FastCDC:
    a gear hash picks the cuts, 4 KB to 64 KB, 16 KB on average), so an edit changes only the chunks around
    it and a moved or copied file changes none. The store keeps chunks/<sha256> once each,
    files/<sha256 of a file> listing a file's chunks, manifest.txt with "hash size path" per file (paths
    from the repository root, sorted), and format ("assets 1"). A push sends only the chunks the store
    lacks; a pull rebuilds each file from the chunks of this checkout's old copies plus the ones it fetches.

    A push stages everything in incoming/ (over ssh: one tar stream, unpacked and checked by the commit.sh
    inside it), checks every chunk against its hash, and replaces the manifest last, so a push that stops
    halfway changes nothing; if another push committed meanwhile it's refused. The replaced manifest moves
    to manifests/; file lists and chunks that no manifest of the last 30 days needs are deleted (the trash).
    A store entry outside this checkout's folders (or ignored) is left as it is.

    .assets/ next to assets.txt is this checkout's state, which its own .gitignore ("*") hides from git: the
    base, the manifest as of the last sync with a store (one per store: base-<store id>.txt), makes the
    compare three-way, so a file changed only elsewhere is pulled, not pushed back, and a store this checkout
    never synced with gets its files as new ones, never as deletions (a branch whose assets.txt names its
    own store).
    Nothing has to be resolved to pull or push: a file changed differently here and in the store keeps both,
    the store's at its path and this checkout's renamed to name.conflict-<checkout folder>-<time>.ext.

    With git: it warns, and asks before pull and push, when git tracks a file in a folder or would add it
    (both would keep a remote copy, which can disagree); it refuses a store that is a git repository or inside
    a work tree. Paths from a store are checked before anything is written: none may leave the repository.
    Over ssh it runs ssh.exe (Windows OpenSSH), and bash, tar and GNU coreutils on the server.
*/

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef int64_t s64;
typedef int32_t bool32;

#define internal static
#define ArrayCount(Array) (sizeof(Array) / sizeof((Array)[0]))
#define Minimum(A, B) ((A) < (B) ? (A) : (B))
#define AssertAlways(Expression) if (!(Expression)) { fprintf(stderr, "%s(%d): assert failed: %s\n", __FILE__, __LINE__, #Expression); fflush(stderr); *(volatile int *)0 = 0; }

struct file_entry
{
    char Hash[65];  // lowercase hex SHA-256 of the file's bytes
    u64 Size;
    u64 WriteTime;  // base.txt only: this checkout's write time when Hash was taken (0: hash it again)
    char *Path;     // from the repository root, '/' separators, UTF-8
};

struct manifest
{
    u32 Count;
    u32 Max;
    file_entry *Entries; // sorted by Path
};

struct chunk
{
    char Hash[65];
    u64 Offset;     // in its source: a file here, or the fetched tar
    u32 Size;
    u32 Source;
};

struct chunk_list
{
    u32 Count;
    u32 Max;
    chunk *Chunks;
};

struct config
{
    char Remote[MAX_PATH];
    u32 FolderCount;
    char Folders[32][MAX_PATH];  // relative to the repository root, '/' separators, "." for all of it
    u32 IgnoreCount;
    char Ignores[64][MAX_PATH];
};

struct folder_entry
{
    char *Name;
    u64 WriteTime;
    bool32 IsFolder;
};

enum change_kind
{
    Change_None,     // the same here and on the remote
    Change_Pull,     // only the remote changed it: pull fetches it, or deletes it here
    Change_Push,     // only this checkout changed it: push sends it, or deletes it on the remote
    Change_KeptHere, // changed here, deleted on the remote: this copy stays, and push adds it back
    Change_Restored, // deleted here, changed on the remote: pull brings the remote's back
    Change_Conflict, // changed differently in both: pull renames this copy and fetches the remote's
};

struct change
{
    change_kind Kind;
    char *Path;
    file_entry *Here;
    file_entry *Base;
    file_entry *Server;
    char *OldCopy;  // pull: where this checkout's previous version of the file is now (its chunks get reused)
    bool32 Failed;  // pull didn't manage it: base.txt keeps the old entry, and push refuses
};

// NOTE: Chunk sizes. Changing them (or the gear table in ChunkFile) only costs deduplication against
//       what's already on the remote: every file lists its own chunks.
#define CHUNK_MIN 4096
#define CHUNK_AVERAGE 16384
#define CHUNK_MAX 65536
#define TRASH_DAYS 30

// NOTE: Every path a file can take here (a repository path plus ".assets-tmp" or a conflict suffix) fits
//       in MAX_PATH; longer ones are refused up front, never truncated.
#define PATH_LIMIT (MAX_PATH - 64)
#define VERSION "0.1.0"
#define STORE_FORMAT "assets 1"

internal int
CompareEntries(const void *A, const void *B)
{
    return strcmp(((file_entry *)A)->Path, ((file_entry *)B)->Path);
}

internal int
CompareChunks(const void *A, const void *B)
{
    return strcmp(((chunk *)A)->Hash, ((chunk *)B)->Hash);
}

internal int
CompareStrings(const void *A, const void *B)
{
    return strcmp(*(char **)A, *(char **)B);
}

internal int
CompareFolderEntries(const void *A, const void *B)
{
    return strcmp(((folder_entry *)A)->Name, ((folder_entry *)B)->Name);
}

internal void
AddEntry(manifest *Manifest, file_entry Entry)
{
    if (Manifest->Count == Manifest->Max)
    {
        Manifest->Max = Manifest->Max ? 2*Manifest->Max : 256;
        Manifest->Entries = (file_entry *)realloc(Manifest->Entries, Manifest->Max*sizeof(file_entry));
        AssertAlways(Manifest->Entries);
    }
    Manifest->Entries[Manifest->Count++] = Entry;
}

internal void
AddChunk(chunk_list *List, chunk Chunk)
{
    if (List->Count == List->Max)
    {
        List->Max = List->Max ? 2*List->Max : 1024;
        List->Chunks = (chunk *)realloc(List->Chunks, List->Max*sizeof(chunk));
        AssertAlways(List->Chunks);
    }
    List->Chunks[List->Count++] = Chunk;
}

internal void
AddString(char ***Strings, u32 *Count, u32 *Max, char *String)
{
    if (*Count == *Max)
    {
        *Max = *Max ? 2*(*Max) : 1024;
        *Strings = (char **)realloc(*Strings, (*Max)*sizeof(char *));
        AssertAlways(*Strings);
    }
    (*Strings)[(*Count)++] = String;
}

internal u32
SortUnique(char **Strings, u32 Count)
{
    if (Count)
    {
        qsort(Strings, Count, sizeof(char *), CompareStrings);
    }
    u32 Result = 0;
    for (u32 Index = 0; Index < Count; ++Index)
    {
        if (!Result || strcmp(Strings[Result - 1], Strings[Index]) != 0)
        {
            Strings[Result++] = Strings[Index];
        }
    }
    return Result;
}

internal bool32
Contains(char **Sorted, u32 Count, char *String)
{
    bool32 Result = Count && bsearch(&String, Sorted, Count, sizeof(char *), CompareStrings);
    return Result;
}

internal file_entry *
FindEntry(manifest *Manifest, char *Path)
{
    file_entry Key = {};
    Key.Path = Path;
    file_entry *Result = Manifest->Count ? (file_entry *)bsearch(&Key, Manifest->Entries, Manifest->Count,
                                                                 sizeof(file_entry), CompareEntries) : 0;
    return Result;
}

internal chunk *
FindChunk(chunk_list *List, char *Hash)
{
    chunk Key = {};
    memcpy(Key.Hash, Hash, sizeof(Key.Hash));
    chunk *Result = List->Count ? (chunk *)bsearch(&Key, List->Chunks, List->Count, sizeof(chunk), CompareChunks) : 0;
    return Result;
}

internal void
Widen(char *Utf8, wchar_t *Out, u32 OutCount)
{
    int Length = MultiByteToWideChar(CP_UTF8, 0, Utf8, -1, Out, (int)OutCount);
    AssertAlways(Length > 0);
}

internal FILE *
OpenFile(char *Path, wchar_t *Mode)
{
    wchar_t WidePath[MAX_PATH] = {};
    Widen(Path, WidePath, MAX_PATH);
    FILE *Result = _wfopen(WidePath, Mode);
    return Result;
}

internal bool32
Exists(char *Path)
{
    wchar_t WidePath[MAX_PATH] = {};
    Widen(Path, WidePath, MAX_PATH);
    bool32 Result = (GetFileAttributesW(WidePath) != INVALID_FILE_ATTRIBUTES);
    return Result;
}

internal void
MakeFolder(char *Path)
{
    wchar_t WidePath[MAX_PATH] = {};
    Widen(Path, WidePath, MAX_PATH);
    CreateDirectoryW(WidePath, 0);
}

internal bool32
DeletePath(char *Path)
{
    wchar_t WidePath[MAX_PATH] = {};
    Widen(Path, WidePath, MAX_PATH);
    bool32 Result = DeleteFileW(WidePath);
    return Result;
}

internal bool32
MovePath(char *From, char *To, DWORD Flags)
{
    wchar_t WideFrom[MAX_PATH] = {};
    wchar_t WideTo[MAX_PATH] = {};
    Widen(From, WideFrom, MAX_PATH);
    Widen(To, WideTo, MAX_PATH);
    bool32 Result = MoveFileExW(WideFrom, WideTo, Flags);
    return Result;
}

internal char *
ReadText(char *Path, u32 *OutSize)
{
    char *Result = 0;
    FILE *File = OpenFile(Path, L"rb");
    if (File)
    {
        _fseeki64(File, 0, SEEK_END);
        s64 Size = _ftelli64(File);
        _fseeki64(File, 0, SEEK_SET);
        Result = (char *)malloc((size_t)Size + 1);
        size_t Read = fread(Result, 1, (size_t)Size, File);
        Result[Read] = 0;
        fclose(File);
        if (OutSize)
        {
            *OutSize = (u32)Read;
        }
    }
    return Result;
}

internal void
HexDigest(u8 *Digest, char *OutHash)
{
    for (u32 ByteIndex = 0; ByteIndex < 32; ++ByteIndex)
    {
        sprintf(OutHash + 2*ByteIndex, "%02x", Digest[ByteIndex]);
    }
}

internal bool32
ReadAt(FILE *File, u64 Offset, u8 *Buffer, u32 Size)
{
    bool32 Result = (_fseeki64(File, (s64)Offset, SEEK_SET) == 0) && (fread(Buffer, 1, Size, File) == Size);
    return Result;
}

internal u64
NowFileTime(void)
{
    FILETIME Now = {};
    GetSystemTimeAsFileTime(&Now);
    u64 Result = ((u64)Now.dwHighDateTime << 32) | Now.dwLowDateTime;
    return Result;
}

internal folder_entry *
ListFolder(char *Folder, u32 *OutCount)
{
    // NOTE: Every name in Folder but . and .., sorted.
    u32 Count = 0;
    u32 Max = 256;
    folder_entry *Result = (folder_entry *)malloc(Max*sizeof(folder_entry));
    char Wildcard[MAX_PATH] = {};
    wchar_t WideWildcard[MAX_PATH] = {};
    snprintf(Wildcard, sizeof(Wildcard), "%s/*", Folder);
    Widen(Wildcard, WideWildcard, MAX_PATH);
    WIN32_FIND_DATAW Found = {};
    HANDLE Find = FindFirstFileW(WideWildcard, &Found);
    if (Find != INVALID_HANDLE_VALUE)
    {
        do
        {
            char Name[MAX_PATH] = {};
            WideCharToMultiByte(CP_UTF8, 0, Found.cFileName, -1, Name, sizeof(Name), 0, 0);
            if (!strcmp(Name, ".") || !strcmp(Name, ".."))
            {
                continue;
            }
            if (Count == Max)
            {
                Max *= 2;
                Result = (folder_entry *)realloc(Result, Max*sizeof(folder_entry));
                AssertAlways(Result);
            }
            folder_entry Entry = {};
            Entry.Name = _strdup(Name);
            Entry.WriteTime = ((u64)Found.ftLastWriteTime.dwHighDateTime << 32) | Found.ftLastWriteTime.dwLowDateTime;
            Entry.IsFolder = (Found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            Result[Count++] = Entry;
        } while (FindNextFileW(Find, &Found));
        FindClose(Find);
    }
    if (Count)
    {
        qsort(Result, Count, sizeof(folder_entry), CompareFolderEntries);
    }
    *OutCount = Count;
    return Result;
}

internal void
DeleteTree(char *Folder)
{
    u32 Count = 0;
    folder_entry *Entries = ListFolder(Folder, &Count);
    for (u32 Index = 0; Index < Count; ++Index)
    {
        char Path[MAX_PATH] = {};
        snprintf(Path, sizeof(Path), "%s/%s", Folder, Entries[Index].Name);
        if (Entries[Index].IsFolder)
        {
            DeleteTree(Path);
        }
        else
        {
            DeletePath(Path);
        }
    }
    wchar_t WideFolder[MAX_PATH] = {};
    Widen(Folder, WideFolder, MAX_PATH);
    RemoveDirectoryW(WideFolder);
}

internal bool32
HashFile(BCRYPT_ALG_HANDLE Algorithm, char *Path, char *OutHash)
{
    bool32 Result = false;
    wchar_t WidePath[MAX_PATH] = {};
    Widen(Path, WidePath, MAX_PATH);
    HANDLE File = CreateFileW(WidePath, GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, 0,
                              OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, 0);
    BCRYPT_HASH_HANDLE Hash = 0;
    if (File != INVALID_HANDLE_VALUE && BCryptCreateHash(Algorithm, &Hash, 0, 0, 0, 0, 0) == 0)
    {
        u32 BufferSize = 1 << 20;
        u8 *Buffer = (u8 *)malloc(BufferSize);
        bool32 ReadFailed = false;
        for (;;)
        {
            DWORD Read = 0;
            if (!ReadFile(File, Buffer, BufferSize, &Read, 0))
            {
                ReadFailed = true;
                break;
            }
            if (Read == 0)
            {
                break;
            }
            BCryptHashData(Hash, Buffer, Read, 0);
        }
        u8 Digest[32] = {};
        if (!ReadFailed && BCryptFinishHash(Hash, Digest, sizeof(Digest), 0) == 0)
        {
            HexDigest(Digest, OutHash);
            Result = true;
        }
        BCryptDestroyHash(Hash);
        free(Buffer);
    }
    if (File != INVALID_HANDLE_VALUE)
    {
        CloseHandle(File);
    }
    return Result;
}

internal bool32
ChunkFile(BCRYPT_ALG_HANDLE Algorithm, char *Path, u32 Source, chunk_list *Out, char *OutFileHash)
{
    // NOTE: FastCDC: a gear hash rolls over the bytes and cuts where its top bits are zero, with a
    //       harder mask before the average size and an easier one after, so sizes cluster around it.
    //       OutFileHash (optional) gets the whole file's hash, from the same read.
    u64 Gear[256] = {};
    u64 Random = 0x5DEECE66DULL;
    for (u32 Index = 0; Index < 256; ++Index)
    {
        Random ^= Random << 13;
        Random ^= Random >> 7;
        Random ^= Random << 17;
        Gear[Index] = Random;
    }
    u64 MaskHard = ((1ULL << 16) - 1) << 48; // 16 bits: log2(CHUNK_AVERAGE) + 2
    u64 MaskEasy = ((1ULL << 12) - 1) << 52; // 12 bits: log2(CHUNK_AVERAGE) - 2

    bool32 Result = false;
    wchar_t WidePath[MAX_PATH] = {};
    Widen(Path, WidePath, MAX_PATH);
    HANDLE File = CreateFileW(WidePath, GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, 0,
                              OPEN_EXISTING, 0, 0);
    LARGE_INTEGER FileSize = {};
    if (File != INVALID_HANDLE_VALUE && GetFileSizeEx(File, &FileSize))
    {
        u64 Size = (u64)FileSize.QuadPart;
        HANDLE Mapping = Size ? CreateFileMappingW(File, 0, PAGE_READONLY, 0, 0, 0) : 0;
        u8 *Data = Mapping ? (u8 *)MapViewOfFile(Mapping, FILE_MAP_READ, 0, 0, 0) : 0;
        BCRYPT_HASH_HANDLE FileHash = 0;
        Result = ((Size == 0) || Data) && (BCryptCreateHash(Algorithm, &FileHash, 0, 0, 0, 0, 0) == 0);
        for (u64 At = 0; Result && At < Size;)
        {
            u64 Left = Size - At;
            u64 Cut = Left;
            if (Left > CHUNK_MIN)
            {
                u64 End = Minimum(Left, (u64)CHUNK_MAX);
                u64 Normal = Minimum(End, (u64)CHUNK_AVERAGE);
                u64 Fingerprint = 0;
                u64 Index = CHUNK_MIN;
                Cut = End;
                for (; Index < Normal; ++Index)
                {
                    Fingerprint = (Fingerprint << 1) + Gear[Data[At + Index]];
                    if (!(Fingerprint & MaskHard))
                    {
                        Cut = Index;
                        break;
                    }
                }
                for (; Cut == End && Index < End; ++Index)
                {
                    Fingerprint = (Fingerprint << 1) + Gear[Data[At + Index]];
                    if (!(Fingerprint & MaskEasy))
                    {
                        Cut = Index;
                    }
                }
            }
            chunk Chunk = {};
            u8 Digest[32] = {};
            BCryptHash(Algorithm, 0, 0, Data + At, (ULONG)Cut, Digest, sizeof(Digest));
            BCryptHashData(FileHash, Data + At, (ULONG)Cut, 0);
            HexDigest(Digest, Chunk.Hash);
            Chunk.Offset = At;
            Chunk.Size = (u32)Cut;
            Chunk.Source = Source;
            AddChunk(Out, Chunk);
            At += Cut;
        }
        if (FileHash)
        {
            u8 Digest[32] = {};
            BCryptFinishHash(FileHash, Digest, sizeof(Digest), 0);
            BCryptDestroyHash(FileHash);
            if (OutFileHash)
            {
                HexDigest(Digest, OutFileHash);
            }
        }
        if (Data)
        {
            UnmapViewOfFile(Data);
        }
        if (Mapping)
        {
            CloseHandle(Mapping);
        }
    }
    if (File != INVALID_HANDLE_VALUE)
    {
        CloseHandle(File);
    }
    return Result;
}

internal bool32
WriteWholeFile(char *Path, char *Data, u32 Size)
{
    // NOTE: Written next to it, then renamed over it, so a crash never leaves half a file.
    char TempPath[MAX_PATH] = {};
    snprintf(TempPath, sizeof(TempPath), "%s.tmp", Path);
    bool32 Result = false;
    FILE *File = OpenFile(TempPath, L"wb");
    if (File)
    {
        bool32 Written = (fwrite(Data, 1, Size, File) == Size);
        Written = (fclose(File) == 0) && Written;
        Result = Written && MovePath(TempPath, Path, MOVEFILE_REPLACE_EXISTING|MOVEFILE_COPY_ALLOWED);
    }
    if (!Result)
    {
        fprintf(stderr, "%s: can't write it\n", Path);
    }
    return Result;
}

internal void
WriteTarEntry(FILE *Tar, char *Name, u8 *Data, u64 Size)
{
    // NOTE: ustar: a 512-byte header, then the data padded to 512 bytes.
    u8 Header[512] = {};
    snprintf((char *)Header, 100, "%s", Name);
    memcpy(Header + 100, "0000644", 8);
    memcpy(Header + 108, "0000000", 8);
    memcpy(Header + 116, "0000000", 8);
    snprintf((char *)Header + 124, 12, "%011llo", Size);
    memcpy(Header + 136, "00000000000", 12);
    Header[156] = '0';
    memcpy(Header + 257, "ustar", 6);
    memcpy(Header + 263, "00", 2);
    memset(Header + 148, ' ', 8);
    u32 Checksum = 0;
    for (u32 Index = 0; Index < sizeof(Header); ++Index)
    {
        Checksum += Header[Index];
    }
    snprintf((char *)Header + 148, 8, "%06o", Checksum);
    fwrite(Header, 1, sizeof(Header), Tar);
    fwrite(Data, 1, (size_t)Size, Tar);
    u8 Zeros[512] = {};
    fwrite(Zeros, 1, (size_t)((512 - (Size & 511)) & 511), Tar);
}

internal bool32
IsHash(char *Text)
{
    // NOTE: Hashes from a store become file names, so exactly 64 lowercase hex digits.
    bool32 Result = (strlen(Text) == 64);
    for (u32 Index = 0; Result && Index < 64; ++Index)
    {
        Result = (Text[Index] >= '0' && Text[Index] <= '9') || (Text[Index] >= 'a' && Text[Index] <= 'f');
    }
    return Result;
}

internal manifest
ParseManifest(char *Text, bool32 HasWriteTime, bool32 *OutFailed)
{
    // NOTE: Each line is "hash size path", or "hash size writetime path" in base.txt.
    manifest Result = {};
    char *At = Text;
    while (At && *At)
    {
        char *End = strchr(At, '\n');
        if (End)
        {
            *End = 0;
        }
        file_entry Entry = {};
        char *Field = At + 65;
        bool32 Valid = (strlen(At) > 66) && (At[64] == ' ');
        if (Valid)
        {
            memcpy(Entry.Hash, At, 64);
            Entry.Size = _strtoui64(Field, &Field, 10);
            if (HasWriteTime)
            {
                Entry.WriteTime = _strtoui64(Field, &Field, 10);
            }
            Valid = (Field[0] == ' ') && Field[1] && IsHash(Entry.Hash);
        }
        // NOTE: A path is written to as it is, so it must stay inside the repository: relative, '/' only,
        //       no "." or ".." names, nothing Windows reads as a drive, stream or device separator.
        for (char *Name = Valid ? Field + 1 : 0; Valid && Name;)
        {
            char *Slash = strchr(Name, '/');
            size_t Length = Slash ? (size_t)(Slash - Name) : strlen(Name);
            Valid = Length && !(Length == 1 && Name[0] == '.') && !(Length == 2 && !strncmp(Name, "..", 2));
            for (size_t Index = 0; Valid && Index < Length; ++Index)
            {
                Valid = (u8)Name[Index] >= 0x20 && !strchr("\\:*?\"<>|", Name[Index]);
            }
            Name = Slash ? Slash + 1 : 0;
        }
        if (Valid)
        {
            Entry.Path = _strdup(Field + 1);
            AddEntry(&Result, Entry);
        }
        else
        {
            *OutFailed = true;
        }
        At = End ? End + 1 : 0;
    }
    if (Result.Count)
    {
        qsort(Result.Entries, Result.Count, sizeof(file_entry), CompareEntries);
    }
    return Result;
}

internal char *
FormatManifest(manifest *Manifest, bool32 HasWriteTime, u32 *OutSize)
{
    u64 Max = 1;
    for (u32 EntryIndex = 0; EntryIndex < Manifest->Count; ++EntryIndex)
    {
        Max += 64 + 3*22 + strlen(Manifest->Entries[EntryIndex].Path);
    }
    char *Result = (char *)malloc(Max);
    char *At = Result;
    for (u32 EntryIndex = 0; EntryIndex < Manifest->Count; ++EntryIndex)
    {
        file_entry *Entry = Manifest->Entries + EntryIndex;
        if (HasWriteTime)
        {
            At += sprintf(At, "%s %llu %llu %s\n", Entry->Hash, Entry->Size, Entry->WriteTime, Entry->Path);
        }
        else
        {
            At += sprintf(At, "%s %llu %s\n", Entry->Hash, Entry->Size, Entry->Path);
        }
    }
    *At = 0;
    *OutSize = (u32)(At - Result);
    return Result;
}

struct pipe_feed
{
    HANDLE Pipe;
    char *Data;
};

internal DWORD WINAPI
FeedPipe(void *Parameter)
{
    // NOTE: Its own thread: a child that answers while it reads (ssh echoing a long list back) would
    //       fill its output pipe and stop reading, while a single thread was still writing.
    pipe_feed *Feed = (pipe_feed *)Parameter;
    size_t Size = strlen(Feed->Data);
    for (size_t At = 0; At < Size;)
    {
        DWORD Written = 0;
        if (!WriteFile(Feed->Pipe, Feed->Data + At, (DWORD)Minimum(Size - At, (size_t)1 << 20), &Written, 0))
        {
            break;
        }
        At += Written;
    }
    CloseHandle(Feed->Pipe); // EOF: bash -s runs the script once its stdin ends
    return 0;
}

internal int
RunProcess(char *CommandLine, char *Input, char *InputPath, char **Output, char *OutputPath, bool32 QuietErrors)
{
    // NOTE: stdin is the Input text or the InputPath file (else NUL); stdout comes back in *Output or
    //       goes to the OutputPath file (else NUL); stderr is ours (NUL if QuietErrors), so ssh's
    //       errors print as they happen.
    SECURITY_ATTRIBUTES Inherit = {sizeof(Inherit), 0, TRUE};
    HANDLE Null = CreateFileW(L"NUL", GENERIC_READ|GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE, &Inherit,
                              OPEN_EXISTING, 0, 0);
    HANDLE Error = 0;
    DuplicateHandle(GetCurrentProcess(), QuietErrors ? Null : GetStdHandle(STD_ERROR_HANDLE), GetCurrentProcess(),
                    &Error, 0, TRUE, DUPLICATE_SAME_ACCESS);
    HANDLE InRead = Null;
    HANDLE InWrite = 0;
    HANDLE OutRead = 0;
    HANDLE OutWrite = Null;
    wchar_t WidePath[MAX_PATH] = {};
    if (Input)
    {
        CreatePipe(&InRead, &InWrite, &Inherit, 1 << 16);
        SetHandleInformation(InWrite, HANDLE_FLAG_INHERIT, 0);
    }
    else if (InputPath)
    {
        Widen(InputPath, WidePath, MAX_PATH);
        InRead = CreateFileW(WidePath, GENERIC_READ, FILE_SHARE_READ, &Inherit, OPEN_EXISTING, 0, 0);
    }
    if (Output)
    {
        CreatePipe(&OutRead, &OutWrite, &Inherit, 1 << 16);
        SetHandleInformation(OutRead, HANDLE_FLAG_INHERIT, 0);
    }
    else if (OutputPath)
    {
        Widen(OutputPath, WidePath, MAX_PATH);
        OutWrite = CreateFileW(WidePath, GENERIC_WRITE, 0, &Inherit, CREATE_ALWAYS, 0, 0);
    }

    STARTUPINFOW Startup = {};
    Startup.cb = sizeof(Startup);
    Startup.dwFlags = STARTF_USESTDHANDLES;
    Startup.hStdInput = InRead;
    Startup.hStdOutput = OutWrite;
    Startup.hStdError = Error;
    PROCESS_INFORMATION Process = {};
    wchar_t WideCommandLine[4096] = {};
    Widen(CommandLine, WideCommandLine, ArrayCount(WideCommandLine));
    bool32 Started = (InRead != INVALID_HANDLE_VALUE) && (OutWrite != INVALID_HANDLE_VALUE) &&
                     CreateProcessW(0, WideCommandLine, 0, 0, TRUE, 0, 0, 0, &Startup, &Process);
    DWORD StartError = GetLastError();
    if (InRead != Null && InRead != INVALID_HANDLE_VALUE)
    {
        CloseHandle(InRead);
    }
    if (OutWrite != Null && OutWrite != INVALID_HANDLE_VALUE)
    {
        CloseHandle(OutWrite);
    }
    CloseHandle(Null);
    CloseHandle(Error);

    int Result = -1;
    HANDLE Feeder = 0;
    pipe_feed Feed = {};
    if (Started && Input)
    {
        Feed.Pipe = InWrite;
        Feed.Data = Input;
        Feeder = CreateThread(0, 0, FeedPipe, &Feed, 0, 0);
        AssertAlways(Feeder);
        InWrite = 0;
    }
    if (Started)
    {
        if (Output)
        {
            u32 Size = 0;
            u32 Max = 1 << 16;
            char *Text = (char *)malloc(Max);
            for (;;)
            {
                if (Max - Size < (1 << 15))
                {
                    Max *= 2;
                    Text = (char *)realloc(Text, Max);
                    AssertAlways(Text);
                }
                DWORD Read = 0;
                if (!ReadFile(OutRead, Text + Size, Max - Size - 1, &Read, 0) || Read == 0)
                {
                    break;
                }
                Size += Read;
            }
            Text[Size] = 0;
            *Output = Text;
        }
        WaitForSingleObject(Process.hProcess, INFINITE);
        if (Feeder)
        {
            // NOTE: A child that quit early leaves the feeder's WriteFile failing on the broken pipe.
            WaitForSingleObject(Feeder, INFINITE);
            CloseHandle(Feeder);
        }
        DWORD ExitCode = 0;
        GetExitCodeProcess(Process.hProcess, &ExitCode);
        Result = (int)ExitCode;
        CloseHandle(Process.hProcess);
        CloseHandle(Process.hThread);
    }
    else if (!QuietErrors)
    {
        fprintf(stderr, "can't run %s (error %lu)\n", CommandLine, StartError);
    }
    if (InWrite)
    {
        CloseHandle(InWrite);
    }
    if (OutRead)
    {
        CloseHandle(OutRead);
    }
    return Result;
}

internal bool32
WildcardMatch(char *Pattern, char *Name)
{
    // NOTE: * matches any run of characters, ? any one; case-insensitive, like Windows names.
    bool32 Result = false;
    if (*Pattern == 0)
    {
        Result = (*Name == 0);
    }
    else if (*Pattern == '*')
    {
        Result = WildcardMatch(Pattern + 1, Name) || (*Name && WildcardMatch(Pattern, Name + 1));
    }
    else if (*Name && (*Pattern == '?' || tolower((u8)*Pattern) == tolower((u8)*Name)))
    {
        Result = WildcardMatch(Pattern + 1, Name + 1);
    }
    return Result;
}

internal bool32
IsLeftOut(config *Config, char *Name)
{
    // NOTE: git's and this tool's own folders never sync; neither do ignored names.
    bool32 Result = !strcmp(Name, ".git") || !strcmp(Name, ".assets");
    for (u32 IgnoreIndex = 0; !Result && IgnoreIndex < Config->IgnoreCount; ++IgnoreIndex)
    {
        Result = WildcardMatch(Config->Ignores[IgnoreIndex], Name);
    }
    u32 Length = (u32)strlen(Name);
    char *TempSuffix = ".assets-tmp";
    Result = Result || (Length > strlen(TempSuffix) && !strcmp(Name + Length - strlen(TempSuffix), TempSuffix));
    return Result;
}

internal bool32
InScope(config *Config, char *Path)
{
    // NOTE: A path (from the repository root) syncs when it's under a folder and no name in it is left out.
    //       assets.txt itself never does: it travels with the repository, not the store.
    char *Rest = 0;
    for (u32 FolderIndex = 0; !Rest && FolderIndex < Config->FolderCount; ++FolderIndex)
    {
        char *Folder = Config->Folders[FolderIndex];
        size_t Length = strlen(Folder);
        if (!strcmp(Folder, "."))
        {
            Rest = Path;
        }
        else if (!strncmp(Path, Folder, Length) && Path[Length] == '/')
        {
            Rest = Path + Length + 1;
        }
    }
    bool32 Result = (Rest != 0) && strcmp(Path, "assets.txt") != 0;
    for (char *Name = Rest; Result && Name && *Name;)
    {
        char Component[MAX_PATH] = {};
        char *Slash = strchr(Name, '/');
        snprintf(Component, sizeof(Component), "%.*s", Slash ? (int)(Slash - Name) : (int)strlen(Name), Name);
        Result = !IsLeftOut(Config, Component);
        Name = Slash ? Slash + 1 : 0;
    }
    return Result;
}

internal manifest
ScanFolders(BCRYPT_ALG_HANDLE Algorithm, char *Root, config *Config, manifest *Cache, bool32 *OutFailed)
{
    // NOTE: A file whose size and write time match its Cache entry keeps that hash; the rest are hashed.
    manifest Result = {};
    u32 PendingCount = 0;
    u32 PendingMax = 0;
    char **Pending = 0; // "" for the root, else "dir/sub/"
    for (u32 FolderIndex = 0; FolderIndex < Config->FolderCount; ++FolderIndex)
    {
        char *Folder = Config->Folders[FolderIndex];
        char Start[MAX_PATH] = {};
        snprintf(Start, sizeof(Start), "%s/", Folder);
        AddString(&Pending, &PendingCount, &PendingMax, _strdup(!strcmp(Folder, ".") ? "" : Start));
    }
    for (u32 PendingIndex = 0; PendingIndex < PendingCount; ++PendingIndex)
    {
        char *Folder = Pending[PendingIndex];
        char Wildcard[MAX_PATH] = {};
        snprintf(Wildcard, sizeof(Wildcard), "%s/%s*", Root, Folder);
        wchar_t WideWildcard[MAX_PATH] = {};
        Widen(Wildcard, WideWildcard, MAX_PATH);
        WIN32_FIND_DATAW Found = {};
        HANDLE Find = FindFirstFileW(WideWildcard, &Found);
        if (Find == INVALID_HANDLE_VALUE)
        {
            // NOTE: A folder that isn't here yet is fine: pull makes it.
            if (GetLastError() != ERROR_PATH_NOT_FOUND && GetLastError() != ERROR_FILE_NOT_FOUND)
            {
                fprintf(stderr, "%s: can't list it\n", Wildcard);
                *OutFailed = true;
            }
            continue;
        }
        do
        {
            char Name[4*MAX_PATH] = {};
            WideCharToMultiByte(CP_UTF8, 0, Found.cFileName, -1, Name, sizeof(Name), 0, 0);
            if (!strcmp(Name, ".") || !strcmp(Name, "..") || IsLeftOut(Config, Name) ||
                (Folder[0] == 0 && !strcmp(Name, "assets.txt")))
            {
                continue;
            }
            size_t FullLength = strlen(Root) + 1 + strlen(Folder) + strlen(Name);
            if (FullLength > PATH_LIMIT)
            {
                // NOTE: Counts as unreadable: skipping it would look like a deletion.
                fprintf(stderr, "%s/%s%s: path too long (%zu characters, at most %d): shorten it\n", Root, Folder, Name,
                        FullLength, PATH_LIMIT);
                *OutFailed = true;
                continue;
            }
            char Path[MAX_PATH] = {};
            snprintf(Path, sizeof(Path), "%s%s", Folder, Name);
            if (Found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            {
                fprintf(stderr, "%s/%s: a link, not synced\n", Root, Path);
                continue;
            }
            if (Found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                char Sub[MAX_PATH] = {};
                snprintf(Sub, sizeof(Sub), "%s/", Path);
                AddString(&Pending, &PendingCount, &PendingMax, _strdup(Sub));
                continue;
            }

            file_entry Entry = {};
            Entry.Path = _strdup(Path);
            Entry.Size = ((u64)Found.nFileSizeHigh << 32) | Found.nFileSizeLow;
            Entry.WriteTime = ((u64)Found.ftLastWriteTime.dwHighDateTime << 32) | Found.ftLastWriteTime.dwLowDateTime;
            file_entry *Cached = FindEntry(Cache, Path);
            if (Cached && Cached->WriteTime && Cached->WriteTime == Entry.WriteTime && Cached->Size == Entry.Size)
            {
                memcpy(Entry.Hash, Cached->Hash, sizeof(Entry.Hash));
            }
            else
            {
                char FullPath[MAX_PATH] = {};
                snprintf(FullPath, sizeof(FullPath), "%s/%s", Root, Path);
                if (!HashFile(Algorithm, FullPath, Entry.Hash))
                {
                    fprintf(stderr, "%s: can't read it\n", FullPath);
                    *OutFailed = true;
                    continue;
                }
            }
            AddEntry(&Result, Entry);
        } while (FindNextFileW(Find, &Found));
        FindClose(Find);
    }
    if (Result.Count)
    {
        qsort(Result.Entries, Result.Count, sizeof(file_entry), CompareEntries);
    }
    return Result;
}

int
main(void)
{
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, 0, _IONBF, 0); // in order with ssh's messages, which go straight to stderr
    int ArgCount = 0;
    wchar_t **WideArgs = CommandLineToArgvW(GetCommandLineW(), &ArgCount);
    char Verb[64] = {};
    if (ArgCount == 2)
    {
        WideCharToMultiByte(CP_UTF8, 0, WideArgs[1], -1, Verb, sizeof(Verb), 0, 0);
    }
    bool32 Status = !strcmp(Verb, "status");
    bool32 Pull = !strcmp(Verb, "pull");
    bool32 Push = !strcmp(Verb, "push");
    bool32 Verify = !strcmp(Verb, "verify");
    if (!strcmp(Verb, "version") || !strcmp(Verb, "--version"))
    {
        printf("assets %s (store format \"%s\")\n", VERSION, STORE_FORMAT);
        return 0;
    }
    if (!Status && !Pull && !Push && !Verify)
    {
        fprintf(stderr, "usage: assets status|pull|push|verify|version\n"
                        "  in a folder at or under one with an assets.txt file, of lines:\n"
                        "    remote origin | HOST:DIR | C:\\folder | \\\\server\\share\\folder\n"
                        "    folder art              (one line per folder, inside the repository)\n"
                        "    ignore *.lock           (names to leave out; * and ? are wildcards)\n");
        return 1;
    }

    // SECTION: find the repository: the nearest folder with an assets.txt, at or above the current one
    char Root[MAX_PATH] = {};
    {
        wchar_t WideCurrent[MAX_PATH] = {};
        GetCurrentDirectoryW(MAX_PATH, WideCurrent);
        WideCharToMultiByte(CP_UTF8, 0, WideCurrent, -1, Root, sizeof(Root), 0, 0);
        for (char *C = Root; *C; ++C)
        {
            *C = (*C == '\\') ? '/' : *C;
        }
        size_t Length = strlen(Root);
        if (Length > 1 && Root[Length - 1] == '/')
        {
            Root[Length - 1] = 0;
        }
        // NOTE: Root has no trailing slash, so a drive's root is "C:" and "C:/assets.txt" is its file.
        for (;;)
        {
            char Candidate[MAX_PATH] = {};
            snprintf(Candidate, sizeof(Candidate), "%s/assets.txt", Root);
            if (Exists(Candidate))
            {
                break;
            }
            char *Slash = strrchr(Root, '/');
            if (!Slash || Slash <= Root + 1)
            {
                fprintf(stderr, "not an assets repository: no assets.txt here or in any folder above\n");
                return 1;
            }
            *Slash = 0;
        }
        if (strlen(Root) > PATH_LIMIT - 32)
        {
            fprintf(stderr, "%s: the repository's path is too long; move it higher up\n", Root);
            return 1;
        }
    }
    char *Checkout = strrchr(Root, '/') ? strrchr(Root, '/') + 1 : Root;

    // SECTION: read assets.txt: remote, folder and ignore lines
    config *Config = (config *)calloc(1, sizeof(config));
    {
        char ConfigPath[MAX_PATH] = {};
        snprintf(ConfigPath, sizeof(ConfigPath), "%s/assets.txt", Root);
        char *Text = ReadText(ConfigPath, 0);
        bool32 Bad = !Text;
        u32 LineNumber = 0;
        for (char *Line = Text; Line && *Line;)
        {
            char *End = strchr(Line, '\n');
            if (End)
            {
                *End = 0;
            }
            ++LineNumber;
            size_t Length = strlen(Line);
            while (Length && (Line[Length - 1] == '\r' || Line[Length - 1] == ' ' || Line[Length - 1] == '\t'))
            {
                Line[--Length] = 0;
            }
            while (*Line == ' ' || *Line == '\t')
            {
                ++Line;
            }
            char *Value = Line;
            while (*Value && *Value != ' ' && *Value != '\t')
            {
                ++Value;
            }
            if (*Value)
            {
                *Value++ = 0;
            }
            while (*Value == ' ' || *Value == '\t')
            {
                ++Value;
            }
            char *Problem = 0;
            if (!*Line || *Line == '#')
            {
            }
            else if (!*Value)
            {
                Problem = "a setting needs a value";
            }
            else if (!strcmp(Line, "remote"))
            {
                Problem = Config->Remote[0] ? "a second remote" : 0;
                snprintf(Config->Remote, sizeof(Config->Remote), "%s", Value);
            }
            else if (!strcmp(Line, "folder"))
            {
                // NOTE: Relative, inside the repository: no absolute paths, drives or "..".
                char Folder[MAX_PATH] = {};
                snprintf(Folder, sizeof(Folder), "%s", Value);
                for (char *C = Folder; *C; ++C)
                {
                    *C = (*C == '\\') ? '/' : *C;
                }
                while (strlen(Folder) > 1 && Folder[strlen(Folder) - 1] == '/')
                {
                    Folder[strlen(Folder) - 1] = 0;
                }
                bool32 Inside = Folder[0] != '/' && !strchr(Folder, ':');
                for (char *Name = Folder; Inside && Name;)
                {
                    char *Slash = strchr(Name, '/');
                    size_t NameLength = Slash ? (size_t)(Slash - Name) : strlen(Name);
                    Inside = NameLength && !(NameLength == 2 && !strncmp(Name, "..", 2)) &&
                             !(NameLength == 1 && Name[0] == '.' && (Slash || Name != Folder));
                    Name = Slash ? Slash + 1 : 0;
                }
                if (!Inside)
                {
                    Problem = "a folder must be inside the repository: a relative path without \"..\"";
                }
                else if (Config->FolderCount == ArrayCount(Config->Folders))
                {
                    Problem = "too many folders";
                }
                else
                {
                    for (u32 FolderIndex = 0; FolderIndex < Config->FolderCount; ++FolderIndex)
                    {
                        char *Other = Config->Folders[FolderIndex];
                        size_t A = strlen(Other);
                        size_t B = strlen(Folder);
                        bool32 Overlap = !strcmp(Other, ".") || !strcmp(Folder, ".") || !strcmp(Other, Folder) ||
                                         (A < B && !strncmp(Folder, Other, A) && Folder[A] == '/') ||
                                         (B < A && !strncmp(Other, Folder, B) && Other[B] == '/');
                        Problem = Overlap ? "folders overlap: one is inside another" : Problem;
                    }
                    snprintf(Config->Folders[Config->FolderCount++], MAX_PATH, "%s", Folder);
                }
            }
            else if (!strcmp(Line, "ignore"))
            {
                if (strchr(Value, '/') || strchr(Value, '\\'))
                {
                    Problem = "ignore takes a file or folder name, not a path";
                }
                else if (Config->IgnoreCount == ArrayCount(Config->Ignores))
                {
                    Problem = "too many ignore lines";
                }
                else
                {
                    snprintf(Config->Ignores[Config->IgnoreCount++], MAX_PATH, "%s", Value);
                }
            }
            else
            {
                Problem = "unknown setting (want remote, folder or ignore)";
            }
            if (Problem)
            {
                fprintf(stderr, "%s(%u): %s\n", ConfigPath, LineNumber, Problem);
                Bad = true;
            }
            Line = End ? End + 1 : 0;
        }
        if (!Bad && (!Config->Remote[0] || !Config->FolderCount))
        {
            fprintf(stderr, "%s: needs a remote line and at least one folder line\n", ConfigPath);
            Bad = true;
        }
        if (Bad)
        {
            return 1;
        }
    }

    // SECTION: this checkout's state, in .assets/ (its .gitignore of "*" hides all of it from git)
    char StateDir[MAX_PATH] = {};
    char BasePath[MAX_PATH] = {};
    char TempDir[MAX_PATH] = {};
    snprintf(StateDir, sizeof(StateDir), "%s/.assets", Root);
    snprintf(TempDir, sizeof(TempDir), "%s/tmp", StateDir);
    MakeFolder(StateDir);
    MakeFolder(TempDir);
    {
        char IgnorePath[MAX_PATH] = {};
        snprintf(IgnorePath, sizeof(IgnorePath), "%s/.gitignore", StateDir);
        char *Ignore = ReadText(IgnorePath, 0);
        if (!Ignore || strcmp(Ignore, "*\n") != 0)
        {
            WriteWholeFile(IgnorePath, "*\n", 2);
        }
    }

    // SECTION: git around this repository, if there is git: its top folder and where .assets sits in it
    char GitTop[MAX_PATH] = {};
    char GitPrefix[MAX_PATH] = {};
    wchar_t GitExe[MAX_PATH] = {};
    if (SearchPathW(0, L"git.exe", 0, MAX_PATH, GitExe, 0))
    {
        char Command[1024] = {};
        char *Reply = 0;
        snprintf(Command, sizeof(Command), "git -C \"%s\" rev-parse --show-toplevel --show-prefix", Root);
        if (RunProcess(Command, 0, 0, &Reply, 0, true) == 0)
        {
            char *Prefix = strchr(Reply, '\n');
            if (Prefix)
            {
                *Prefix++ = 0;
                char *End = strchr(Prefix, '\n');
                if (End)
                {
                    *End = 0;
                }
                snprintf(GitTop, sizeof(GitTop), "%s", Reply);
                snprintf(GitPrefix, sizeof(GitPrefix), "%s", Prefix);
            }
        }
    }

    // SECTION: the remote: "origin" (git's), a folder (local, mapped drive, \\share), or HOST:DIR over ssh
    char Remote[MAX_PATH] = {};
    snprintf(Remote, sizeof(Remote), "%s", Config->Remote);
    bool32 FromOrigin = !strcmp(Remote, "origin");
    if (FromOrigin)
    {
        char Command[1024] = {};
        char *Url = 0;
        snprintf(Command, sizeof(Command), "git -C \"%s\" remote get-url origin", Root);
        if (!GitTop[0] || RunProcess(Command, 0, 0, &Url, 0, true) != 0)
        {
            fprintf(stderr, "remote origin: this isn't a git repository with an origin remote\n");
            return 1;
        }
        char *End = strpbrk(Url, "\r\n");
        if (End)
        {
            *End = 0;
        }
        if (!strncmp(Url, "file://", 7))
        {
            Url += 7;
        }
        if (!strncmp(Url, "ssh://", 6))
        {
            char *Path = strchr(Url + 6, '/');
            char *Port = strchr(Url + 6, ':');
            if (!Path || (Port && Port < Path))
            {
                fprintf(stderr, "remote origin: %s: ssh URLs with a port aren't supported; write HOST:DIR\n", Url);
                return 1;
            }
            snprintf(Remote, sizeof(Remote), "%.*s:%s", (int)(Path - Url - 6), Url + 6, Path);
        }
        else if (strstr(Url, "://"))
        {
            fprintf(stderr, "remote origin: %s: assets needs ssh (HOST:DIR) or a folder\n", Url);
            return 1;
        }
        else if (Url[0] == '/' && isalpha((u8)Url[1]) && (Url[2] == '/' || !Url[2]))
        {
            // NOTE: Git Bash's /c/... form
            snprintf(Remote, sizeof(Remote), "%c:%s", Url[1], Url + 2);
        }
        else
        {
            snprintf(Remote, sizeof(Remote), "%s", Url);
        }
    }
    for (char *C = Remote; *C; ++C)
    {
        *C = (*C == '\\') ? '/' : *C;
    }
    bool32 Local = (isalpha((u8)Remote[0]) && Remote[1] == ':' && (Remote[2] == '/' || !Remote[2])) ||
                   !strncmp(Remote, "//", 2);
    char Host[256] = {};
    char Dir[MAX_PATH] = {};
    if (Local)
    {
        snprintf(Dir, sizeof(Dir), "%s", Remote);
    }
    else
    {
        char *Colon = strchr(Remote, ':');
        if (Colon)
        {
            snprintf(Host, sizeof(Host), "%.*s", (int)(Colon - Remote), Remote);
            snprintf(Dir, sizeof(Dir), "%s", Colon + 1);
        }
        // NOTE: Host and Dir go into commands and shell scripts, so only plain characters.
        bool32 Plain = Host[0] && Dir[0] && !strstr(Dir, "..");
        for (char *C = Host; *C; ++C)
        {
            Plain = Plain && (isalnum((u8)*C) || strchr("._@-", *C));
        }
        for (char *C = Dir; *C; ++C)
        {
            Plain = Plain && (isalnum((u8)*C) || strchr("._/-~", *C));
        }
        if (!Plain)
        {
            fprintf(stderr, "remote %s: want origin, a folder (C:\\..., \\\\server\\...) or HOST:DIR of letters, digits and ._-/~ \n",
                    Remote);
            return 1;
        }
    }
    while (strlen(Dir) > 1 && Dir[strlen(Dir) - 1] == '/')
    {
        Dir[strlen(Dir) - 1] = 0;
    }
    char Ssh[MAX_PATH] = "ssh.exe";
    {
        // NOTE: Windows' own OpenSSH by its full path: from Git Bash, PATH finds Git's msys ssh first. Its
        //       remote command keeps any quotes, so remote commands here are plain words.
        wchar_t SystemDir[MAX_PATH] = {};
        GetSystemDirectoryW(SystemDir, MAX_PATH);
        char System[MAX_PATH] = {};
        WideCharToMultiByte(CP_UTF8, 0, SystemDir, -1, System, sizeof(System), 0, 0);
        char OpenSsh[MAX_PATH] = {};
        snprintf(OpenSsh, sizeof(OpenSsh), "%s\\OpenSSH\\ssh.exe", System);
        if (Exists(OpenSsh))
        {
            snprintf(Ssh, sizeof(Ssh), "\"%s\"", OpenSsh);
        }
    }
    char SshCommand[512] = {};
    snprintf(SshCommand, sizeof(SshCommand), "%s -o Compression=yes %s bash -s", Ssh, Host);

    // SECTION: resolve the remote's folder (origin's repo, or repo.git), refuse git's own space, read its manifest
    // NOTE: ServerHash is the manifest's hash as read, which push checks again just before replacing it.
    char Script[8192] = {};
    char ServerHash[65] = "none";
    char *ServerText = 0;
    char Format[64] = {};
    {
        char *Refusal = 0;
        if (Local)
        {
            if (FromOrigin)
            {
                char WithGit[MAX_PATH] = {};
                snprintf(WithGit, sizeof(WithGit), "%s.git", Dir);
                if (!Exists(Dir) && Exists(WithGit))
                {
                    snprintf(Dir, sizeof(Dir), "%s", WithGit);
                }
                strcat(Dir, "/assets");
            }
            char Head[MAX_PATH] = {};
            char Objects[MAX_PATH] = {};
            snprintf(Head, sizeof(Head), "%s/HEAD", Dir);
            snprintf(Objects, sizeof(Objects), "%s/objects", Dir);
            char Existing[MAX_PATH] = {};
            snprintf(Existing, sizeof(Existing), "%s", Dir);
            while (!Exists(Existing) && strrchr(Existing, '/'))
            {
                *strrchr(Existing, '/') = 0;
            }
            char Command[1024] = {};
            char *Inside = 0;
            snprintf(Command, sizeof(Command), "git -C \"%s\" rev-parse --is-inside-work-tree", Existing);
            // NOTE: Both as long full paths (no 8.3 names like PROGRA~1), compared case-insensitively.
            char Full[MAX_PATH] = {};
            char LongRoot[MAX_PATH] = {};
            for (u32 Pass = 0; Pass < 2; ++Pass)
            {
                wchar_t WidePath[MAX_PATH] = {};
                wchar_t WideFull[MAX_PATH] = {};
                wchar_t WideLong[MAX_PATH] = {};
                char *Out = Pass ? LongRoot : Full;
                Widen(Pass ? Root : Existing, WidePath, MAX_PATH);
                GetFullPathNameW(WidePath, MAX_PATH, WideFull, 0);
                if (!GetLongPathNameW(WideFull, WideLong, MAX_PATH))
                {
                    wcscpy(WideLong, WideFull);
                }
                WideCharToMultiByte(CP_UTF8, 0, WideLong, -1, Out, MAX_PATH, 0, 0);
                for (char *C = Out; *C; ++C)
                {
                    *C = (*C == '\\') ? '/' : *C;
                }
            }
            strncat(Full, Dir + strlen(Existing), sizeof(Full) - strlen(Full) - 1);
            size_t RootLength = strlen(LongRoot);
            if (Exists(Head) && Exists(Objects))
            {
                Refusal = "it's a git repository itself";
            }
            else if (GitExe[0] && Existing[0] && RunProcess(Command, 0, 0, &Inside, 0, true) == 0 && !strncmp(Inside, "true", 4))
            {
                Refusal = "it's inside a git work tree";
            }
            else if (!_strnicmp(Full, LongRoot, RootLength) && (Full[RootLength] == '/' || !Full[RootLength]))
            {
                Refusal = "it's inside this repository";
            }
            else if (strlen(Dir) > MAX_PATH - 140)
            {
                // NOTE: Its longest path is incoming/<id>/files/<hash>.tmp, about 135 characters below it.
                Refusal = "its path is too long";
            }
            char ManifestPath[MAX_PATH] = {};
            snprintf(ManifestPath, sizeof(ManifestPath), "%s/manifest.txt", Dir);
            ServerText = ReadText(ManifestPath, 0);
            char FormatPath[MAX_PATH] = {};
            snprintf(FormatPath, sizeof(FormatPath), "%s/format", Dir);
            char *FormatText = ReadText(FormatPath, 0);
            if (FormatText)
            {
                snprintf(Format, sizeof(Format), "%.*s", (int)strcspn(FormatText, "\r\n"), FormatText);
            }
        }
        else
        {
            char *Reply = 0;
            snprintf(Script, sizeof(Script),
                     "d=%s\n"
                     "%s"
                     "if [ -f \"$d/HEAD\" ] && [ -d \"$d/objects\" ]; then echo gitrepo; exit 0; fi\n"
                     "p=\"$d\"; while [ ! -d \"$p\" ]; do p=$(dirname \"$p\"); done\n"
                     "if [ \"$(git -C \"$p\" rev-parse --is-inside-work-tree 2>/dev/null)\" = true ]; then echo worktree; exit 0; fi\n"
                     "echo \"dir $d\"\n"
                     "if [ -f \"$d/format\" ]; then echo \"format $(head -n 1 \"$d/format\")\"; fi\n"
                     "if [ -f \"$d/manifest.txt\" ]; then sha256sum < \"$d/manifest.txt\" | cut -c1-64; cat \"$d/manifest.txt\"; fi\n",
                     Dir, FromOrigin ? "[ -d \"$d\" ] || [ ! -d \"$d.git\" ] || d=\"$d.git\"\nd=\"$d/assets\"\n" : "");
            if (RunProcess(SshCommand, Script, 0, &Reply, 0, false) != 0)
            {
                fprintf(stderr, "%s: can't reach it\n", Remote);
                return 1;
            }
            if (!strncmp(Reply, "gitrepo", 7))
            {
                Refusal = "it's a git repository itself";
            }
            else if (!strncmp(Reply, "worktree", 8))
            {
                Refusal = "it's inside a git work tree";
            }
            else if (!strncmp(Reply, "dir ", 4) && strchr(Reply, '\n'))
            {
                char *Line = strchr(Reply, '\n');
                *Line++ = 0;
                snprintf(Dir, sizeof(Dir), "%s", Reply + 4);
                if (!strncmp(Line, "format ", 7) && strchr(Line, '\n'))
                {
                    char *Next = strchr(Line, '\n');
                    *Next++ = 0;
                    snprintf(Format, sizeof(Format), "%s", Line + 7);
                    Line = Next;
                }
                if (*Line)
                {
                    if (strlen(Line) < 65 || Line[64] != '\n')
                    {
                        fprintf(stderr, "%s: unexpected reply: %.200s\n", Remote, Line);
                        return 1;
                    }
                    memcpy(ServerHash, Line, 64);
                    ServerText = Line + 65;
                }
            }
            else
            {
                fprintf(stderr, "%s: unexpected reply: %.200s\n", Remote, Reply);
                return 1;
            }
        }
        if (Refusal)
        {
            fprintf(stderr, "remote %s: %s; keep the assets in a folder of their own, like <repo>.git/assets\n",
                    Dir, Refusal);
            return 1;
        }
        // NOTE: A store without a format file predates it and is format 1; push writes the file.
        if (Format[0] && strcmp(Format, STORE_FORMAT) != 0)
        {
            fprintf(stderr, "remote %s: its format is \"%s\", this version (%s) knows \"%s\": update assets\n", Dir,
                    Format, VERSION, STORE_FORMAT);
            return 1;
        }
    }
    BCRYPT_ALG_HANDLE Algorithm = 0;
    AssertAlways(BCryptOpenAlgorithmProvider(&Algorithm, BCRYPT_SHA256_ALGORITHM, 0, 0) == 0);

    // NOTE: One base per remote (base-<its id>.txt): a checkout whose .assets now names another remote (a
    //       branch with its own store) starts that remote without a base, so its files count as new there
    //       instead of as deleted. A base.txt from before this keeps serving the remote it was made for.
    {
        char Identity[2*MAX_PATH] = {};
        snprintf(Identity, sizeof(Identity), "%s:%s", Local ? "folder" : Host, Dir);
        for (char *C = Identity; Local && *C; ++C)
        {
            *C = (char)tolower((u8)*C);
        }
        u8 Digest[32] = {};
        char Hex[65] = {};
        BCryptHash(Algorithm, 0, 0, (u8 *)Identity, (ULONG)strlen(Identity), Digest, sizeof(Digest));
        HexDigest(Digest, Hex);
        char OldBase[MAX_PATH] = {};
        snprintf(OldBase, sizeof(OldBase), "%s/base.txt", StateDir);
        snprintf(BasePath, sizeof(BasePath), "%s/base-%.16s.txt", StateDir, Hex);
        if (!Exists(BasePath) && Exists(OldBase))
        {
            MovePath(OldBase, BasePath, 0);
        }
    }
    if (Local && ServerText)
    {
        char ManifestPath[MAX_PATH] = {};
        snprintf(ManifestPath, sizeof(ManifestPath), "%s/manifest.txt", Dir);
        if (!HashFile(Algorithm, ManifestPath, ServerHash))
        {
            fprintf(stderr, "%s: can't read it\n", ManifestPath);
            return 1;
        }
    }
    bool32 ServerBad = false;
    manifest ServerAll = ParseManifest(ServerText ? ServerText : (char *)"", false, &ServerBad);
    if (ServerBad)
    {
        fprintf(stderr, "%s/manifest.txt: malformed or unsafe lines (a path leaving the repository?); not touching anything\n", Dir);
        return 1;
    }
    // NOTE: The remote's entries outside this checkout's folders (or ignored) are left as they are.
    manifest Server = {};
    manifest ServerOut = {};
    for (u32 EntryIndex = 0; EntryIndex < ServerAll.Count; ++EntryIndex)
    {
        AddEntry(InScope(Config, ServerAll.Entries[EntryIndex].Path) ? &Server : &ServerOut, ServerAll.Entries[EntryIndex]);
    }
    for (u32 EntryIndex = 0; EntryIndex < Server.Count; ++EntryIndex)
    {
        size_t FullLength = strlen(Root) + 1 + strlen(Server.Entries[EntryIndex].Path);
        if (FullLength > PATH_LIMIT)
        {
            fprintf(stderr, "%s/%s: path too long here (%zu characters, at most %d): move this checkout higher up\n",
                    Root, Server.Entries[EntryIndex].Path, FullLength, PATH_LIMIT);
            return 1;
        }
    }

    // SECTION: verify: every chunk against its hash, every file of the manifest rebuilt from its chunks
    if (Verify)
    {
        bool32 Bad = false;
        if (Local)
        {
            char Folder[MAX_PATH] = {};
            snprintf(Folder, sizeof(Folder), "%s/chunks", Dir);
            u32 ChunkCount = 0;
            folder_entry *Chunks = ListFolder(Folder, &ChunkCount);
            for (u32 Index = 0; Index < ChunkCount; ++Index)
            {
                char Path[MAX_PATH] = {};
                char Hash[65] = {};
                snprintf(Path, sizeof(Path), "%s/%s", Folder, Chunks[Index].Name);
                if (!HashFile(Algorithm, Path, Hash) || strcmp(Hash, Chunks[Index].Name) != 0)
                {
                    printf("bad chunk %s\n", Chunks[Index].Name);
                    Bad = true;
                }
            }
            u8 *Buffer = (u8 *)malloc(CHUNK_MAX);
            for (u32 EntryIndex = 0; EntryIndex < ServerAll.Count; ++EntryIndex)
            {
                file_entry *Entry = ServerAll.Entries + EntryIndex;
                char ListPath[MAX_PATH] = {};
                snprintf(ListPath, sizeof(ListPath), "%s/files/%s", Dir, Entry->Hash);
                char *List = ReadText(ListPath, 0);
                if (!List)
                {
                    printf("no chunk list: %s\n", Entry->Path);
                    Bad = true;
                    continue;
                }
                BCRYPT_HASH_HANDLE Hash = 0;
                BCryptCreateHash(Algorithm, &Hash, 0, 0, 0, 0, 0);
                for (char *Line = strtok(List, "\n"); Line; Line = strtok(0, "\n"))
                {
                    char Path[MAX_PATH] = {};
                    snprintf(Path, sizeof(Path), "%s/chunks/%s", Dir, Line);
                    FILE *File = OpenFile(Path, L"rb");
                    size_t Read = File ? fread(Buffer, 1, CHUNK_MAX, File) : 0;
                    if (File)
                    {
                        fclose(File);
                    }
                    BCryptHashData(Hash, Buffer, (ULONG)Read, 0);
                }
                u8 Digest[32] = {};
                char Got[65] = {};
                BCryptFinishHash(Hash, Digest, sizeof(Digest), 0);
                BCryptDestroyHash(Hash);
                HexDigest(Digest, Got);
                if (strcmp(Got, Entry->Hash) != 0)
                {
                    printf("doesn't match its chunks: %s\n", Entry->Path);
                    Bad = true;
                }
                free(List);
            }
            printf("%u files and %u chunks checked\n", ServerAll.Count, ChunkCount);
        }
        else
        {
            snprintf(Script, sizeof(Script),
                     "export LC_ALL=C\n"
                     "cd %s 2>/dev/null && [ -f manifest.txt ] || { echo 'nothing on the remote yet'; exit 0; }\n"
                     "bad=0\n"
                     "find chunks -type f 2>/dev/null | xargs -r sha256sum | awk '{ n = split($2, p, \"/\"); if ($1 != p[n]) { print \"bad chunk \" p[n]; bad = 1 } } END { exit bad }' || bad=1\n"
                     "while read h size path; do\n"
                     "  if [ ! -f files/$h ]; then echo \"no chunk list: $path\"; bad=1; continue; fi\n"
                     "  got=$(sed 's|^|chunks/|' files/$h | xargs -r cat 2>/dev/null | sha256sum | cut -c1-64)\n"
                     "  [ \"$got\" = \"$h\" ] || { echo \"doesn't match its chunks: $path\"; bad=1; }\n"
                     "done < manifest.txt\n"
                     "echo \"$(wc -l < manifest.txt) files and $(ls chunks 2>/dev/null | wc -l) chunks checked\"\n"
                     "exit $bad\n", Dir);
            char *Report = 0;
            Bad = (RunProcess(SshCommand, Script, 0, &Report, 0, false) != 0);
            printf("%s", Report ? Report : "");
        }
        printf(Bad ? "the remote copy has problems (above)\n" : "the remote copy is intact\n");
        return Bad ? 1 : 0;
    }

    // SECTION: this checkout: base.txt (its last sync, within the folders) and what's in the folders now
    manifest Base = {};
    {
        char *Text = ReadText(BasePath, 0);
        if (Text)
        {
            bool32 BaseBad = false;
            manifest Parsed = ParseManifest(Text, true, &BaseBad);
            if (BaseBad)
            {
                // NOTE: Safe to lose: without a base every difference is a conflict, and both copies stay.
                fprintf(stderr, "%s: malformed; going on without it\n", BasePath);
            }
            for (u32 EntryIndex = 0; !BaseBad && EntryIndex < Parsed.Count; ++EntryIndex)
            {
                if (InScope(Config, Parsed.Entries[EntryIndex].Path))
                {
                    AddEntry(&Base, Parsed.Entries[EntryIndex]);
                }
            }
        }
    }
    bool32 ScanFailed = false;
    manifest Here = ScanFolders(Algorithm, Root, Config, &Base, &ScanFailed);

    // SECTION: git: a folder's files that git tracks, or would add, get a second remote copy (warn; ask to go on)
    if (GitTop[0])
    {
        u32 TrackedTotal = 0;
        u32 UntrackedTotal = 0;
        bool32 Unignored[32] = {};
        for (u32 FolderIndex = 0; FolderIndex < Config->FolderCount; ++FolderIndex)
        {
            char *Folder = Config->Folders[FolderIndex];
            u32 Counts[2] = {};
            for (u32 Pass = 0; Pass < 2; ++Pass)
            {
                char Command[1024] = {};
                char *Files = 0;
                snprintf(Command, sizeof(Command), "git -C \"%s\" ls-files -z %s -- \"%s\"", Root,
                         Pass ? "--others --exclude-standard" : "", Folder);
                if (RunProcess(Command, 0, 0, &Files, 0, true) != 0)
                {
                    continue;
                }
                for (char *File = Files; *File; File += strlen(File) + 1)
                {
                    Counts[Pass] += InScope(Config, File) ? 1 : 0;
                }
            }
            if (Counts[0])
            {
                printf("%s: git tracks %u of its files (git rm -r --cached \"%s\" stops that, keeping the files)\n",
                       Folder, Counts[0], Folder);
            }
            if (Counts[1])
            {
                printf("%s: %u of its files aren't ignored by git (git add would take them)\n", Folder, Counts[1]);
                Unignored[FolderIndex] = true;
            }
            TrackedTotal += Counts[0];
            UntrackedTotal += Counts[1];
        }
        if (TrackedTotal || UntrackedTotal)
        {
            printf("Both git and assets would keep a remote copy of these files: the data is stored twice, and\n"
                   "the two copies can disagree.\n");
            if (Pull || Push)
            {
                printf("Continue? [y]es, [n]o, [a]dd the folders to .gitignore and continue: ");
                char Answer[64] = {};
                if (!fgets(Answer, sizeof(Answer), stdin))
                {
                    fprintf(stderr, "\nno answer (no console): stopping\n");
                    return 1;
                }
                char Choice = (char)tolower((u8)Answer[0]);
                if (Choice == 'a')
                {
                    char IgnorePath[MAX_PATH] = {};
                    snprintf(IgnorePath, sizeof(IgnorePath), "%s/.gitignore", GitTop);
                    u32 OldSize = 0;
                    char *Old = ReadText(IgnorePath, &OldSize);
                    u32 Max = OldSize + 64*MAX_PATH;
                    char *New = (char *)malloc(Max);
                    char *At = New + snprintf(New, Max, "%s%s", Old ? Old : "", (OldSize && Old[OldSize - 1] != '\n') ? "\n" : "");
                    for (u32 FolderIndex = 0; FolderIndex < Config->FolderCount; ++FolderIndex)
                    {
                        char *Folder = Config->Folders[FolderIndex];
                        if (!Unignored[FolderIndex])
                        {
                            continue;
                        }
                        if (!strcmp(Folder, ".") && !GitPrefix[0])
                        {
                            printf("%s: not adding it: that would ignore the whole git repository\n", Folder);
                            continue;
                        }
                        At += sprintf(At, "/%s%s/\n", GitPrefix, strcmp(Folder, ".") ? Folder : "");
                        printf("added /%s%s/ to %s\n", GitPrefix, strcmp(Folder, ".") ? Folder : "", IgnorePath);
                    }
                    if (!WriteWholeFile(IgnorePath, New, (u32)(At - New)))
                    {
                        return 1;
                    }
                }
                else if (Choice != 'y')
                {
                    printf("stopped\n");
                    return 1;
                }
            }
        }
    }

    // SECTION: three-way compare of every path here, in base.txt and on the remote
    u32 ChangeCount = 0;
    change *Changes = (change *)calloc(Here.Count + Base.Count + Server.Count + 1, sizeof(change));
    {
        u32 HereIndex = 0;
        u32 BaseIndex = 0;
        u32 ServerIndex = 0;
        for (;;)
        {
            char *Path = 0;
            char *Candidates[3] = {
                HereIndex < Here.Count ? Here.Entries[HereIndex].Path : 0,
                BaseIndex < Base.Count ? Base.Entries[BaseIndex].Path : 0,
                ServerIndex < Server.Count ? Server.Entries[ServerIndex].Path : 0,
            };
            for (u32 CandidateIndex = 0; CandidateIndex < 3; ++CandidateIndex)
            {
                if (Candidates[CandidateIndex] && (!Path || strcmp(Candidates[CandidateIndex], Path) < 0))
                {
                    Path = Candidates[CandidateIndex];
                }
            }
            if (!Path)
            {
                break;
            }
            change *Change = Changes + ChangeCount++;
            Change->Path = Path;
            Change->Here = (Candidates[0] && !strcmp(Candidates[0], Path)) ? Here.Entries + HereIndex++ : 0;
            Change->Base = (Candidates[1] && !strcmp(Candidates[1], Path)) ? Base.Entries + BaseIndex++ : 0;
            Change->Server = (Candidates[2] && !strcmp(Candidates[2], Path)) ? Server.Entries + ServerIndex++ : 0;
            char *HereHash = Change->Here ? Change->Here->Hash : "";
            char *BaseHash = Change->Base ? Change->Base->Hash : "";
            char *ServerHashOfPath = Change->Server ? Change->Server->Hash : "";
            if (!strcmp(HereHash, ServerHashOfPath))
            {
                Change->Kind = Change_None;
            }
            else if (!strcmp(HereHash, BaseHash))
            {
                Change->Kind = Change_Pull;
            }
            else if (!strcmp(ServerHashOfPath, BaseHash))
            {
                Change->Kind = Change_Push;
            }
            else if (!Change->Server)
            {
                Change->Kind = Change_KeptHere;
            }
            else if (!Change->Here)
            {
                Change->Kind = Change_Restored;
            }
            else
            {
                Change->Kind = Change_Conflict;
            }
        }
    }

    // SECTION: print the changes (status stops here)
    u32 InSyncCount = 0;
    for (u32 ChangeIndex = 0; ChangeIndex < ChangeCount; ++ChangeIndex)
    {
        change *Change = Changes + ChangeIndex;
        switch (Change->Kind)
        {
            case Change_None: ++InSyncCount; break;
            case Change_Pull: printf("pull      %c %s\n", !Change->Here ? 'A' : !Change->Server ? 'D' : 'M', Change->Path); break;
            case Change_Push: printf("push      %c %s\n", !Change->Server ? 'A' : !Change->Here ? 'D' : 'M', Change->Path); break;
            case Change_KeptHere: printf("kept        %s (deleted on the remote, changed here: push adds it back)\n", Change->Path); break;
            case Change_Restored: printf("restored    %s (deleted here, changed on the remote: pull brings it back)\n", Change->Path); break;
            case Change_Conflict: printf("conflict    %s (changed here and on the remote: pull keeps both)\n", Change->Path); break;
        }
    }
    printf("%u files the same here and on the remote\n", InSyncCount);
    if (ScanFailed)
    {
        // NOTE: A file that couldn't be read looks deleted, which a push would send to the remote.
        fprintf(stderr, "some files couldn't be read; not pulling or pushing\n");
        return 1;
    }

    // SECTION: pull: deletes first (a case-only rename on the remote is a delete and an add of the same file here)
    bool32 PullFailed = false;
    if (Pull || Push)
    {
        // NOTE: A deleted file moves into .assets/tmp until the pull ends: a file the remote renamed is
        //       rebuilt from it instead of fetched.
        for (u32 ChangeIndex = 0; ChangeIndex < ChangeCount; ++ChangeIndex)
        {
            change *Change = Changes + ChangeIndex;
            if (Change->Kind == Change_Pull && !Change->Server)
            {
                char FullPath[MAX_PATH] = {};
                char AsidePath[MAX_PATH] = {};
                snprintf(FullPath, sizeof(FullPath), "%s/%s", Root, Change->Path);
                snprintf(AsidePath, sizeof(AsidePath), "%s/deleted-%u", TempDir, ChangeIndex);
                if (!MovePath(FullPath, AsidePath, MOVEFILE_REPLACE_EXISTING))
                {
                    fprintf(stderr, "%s: can't delete it (error %lu)\n", FullPath, GetLastError());
                    Change->Failed = true;
                    continue;
                }
                Change->OldCopy = _strdup(AsidePath);
                // NOTE: Emptied folders go too (the packer takes an existing sources/<pack>/ as the pack's), up
                //       to the configured folder.
                char *Stop = FullPath + strlen(Root) + 1;
                for (u32 FolderIndex = 0; FolderIndex < Config->FolderCount; ++FolderIndex)
                {
                    char *Folder = Config->Folders[FolderIndex];
                    size_t Length = strlen(Folder);
                    if (strcmp(Folder, ".") && !strncmp(Change->Path, Folder, Length) && Change->Path[Length] == '/')
                    {
                        Stop += Length;
                    }
                }
                for (char *Slash = strrchr(FullPath, '/'); Slash && Slash > Stop; Slash = strrchr(FullPath, '/'))
                {
                    *Slash = 0;
                    wchar_t WideFolder[MAX_PATH] = {};
                    Widen(FullPath, WideFolder, MAX_PATH);
                    if (!RemoveDirectoryW(WideFolder))
                    {
                        break;
                    }
                }
            }
        }

        // --- conflicts: this checkout's copy moves aside, to name.conflict-CHECKOUT-time.ext ---
        SYSTEMTIME Now = {};
        GetLocalTime(&Now);
        char Stamp[MAX_PATH] = {};
        snprintf(Stamp, sizeof(Stamp), "%.16s-%04u%02u%02u-%02u%02u%02u", Checkout, Now.wYear, Now.wMonth, Now.wDay,
                 Now.wHour, Now.wMinute, Now.wSecond);
        for (u32 ChangeIndex = 0; ChangeIndex < ChangeCount; ++ChangeIndex)
        {
            change *Change = Changes + ChangeIndex;
            if (Change->Kind == Change_Conflict)
            {
                char *Slash = strrchr(Change->Path, '/');
                char *Dot = strrchr(Slash ? Slash + 1 : Change->Path, '.');
                bool32 HasExtension = Dot && Dot != (Slash ? Slash + 1 : Change->Path);
                char ConflictPath[MAX_PATH] = {};
                if (HasExtension)
                {
                    snprintf(ConflictPath, sizeof(ConflictPath), "%.*s.conflict-%s%s", (int)(Dot - Change->Path),
                             Change->Path, Stamp, Dot);
                }
                else
                {
                    snprintf(ConflictPath, sizeof(ConflictPath), "%s.conflict-%s", Change->Path, Stamp);
                }
                char FromPath[MAX_PATH] = {};
                char ToPath[MAX_PATH] = {};
                snprintf(FromPath, sizeof(FromPath), "%s/%s", Root, Change->Path);
                snprintf(ToPath, sizeof(ToPath), "%s/%s", Root, ConflictPath);
                if (!MovePath(FromPath, ToPath, 0))
                {
                    fprintf(stderr, "%s: can't rename it to %s (error %lu)\n", FromPath, ConflictPath, GetLastError());
                    Change->Failed = true;
                    continue;
                }
                Change->OldCopy = _strdup(ToPath);
                printf("kept both: %s is now %s\n", Change->Path, ConflictPath);
            }
            else if (Change->Kind == Change_Pull && Change->Here && Change->Server)
            {
                char FullPath[MAX_PATH] = {};
                snprintf(FullPath, sizeof(FullPath), "%s/%s", Root, Change->Path);
                Change->OldCopy = _strdup(FullPath);
            }
        }

        // --- the chunk list of every file to fetch ---
        u32 DownloadCount = 0;
        char **Downloads = (char **)malloc((ChangeCount + 1)*sizeof(char *));
        for (u32 ChangeIndex = 0; ChangeIndex < ChangeCount; ++ChangeIndex)
        {
            change *Change = Changes + ChangeIndex;
            bool32 Download = ((Change->Kind == Change_Pull && Change->Server) || Change->Kind == Change_Restored ||
                               Change->Kind == Change_Conflict) && !Change->Failed;
            if (Download)
            {
                Downloads[DownloadCount++] = Change->Server->Hash;
            }
        }
        DownloadCount = SortUnique(Downloads, DownloadCount);
        // NOTE: Lists is "== filehash" lines, each followed by that file's chunk hashes.
        char *Lists = 0;
        if (DownloadCount && Local)
        {
            u32 Size = 0;
            u32 Max = 1 << 16;
            Lists = (char *)malloc(Max);
            for (u32 DownloadIndex = 0; DownloadIndex < DownloadCount; ++DownloadIndex)
            {
                char ListPath[MAX_PATH] = {};
                snprintf(ListPath, sizeof(ListPath), "%s/files/%s", Dir, Downloads[DownloadIndex]);
                u32 ListSize = 0;
                char *List = ReadText(ListPath, &ListSize);
                if (!List)
                {
                    continue;
                }
                while (Max - Size < ListSize + 128)
                {
                    Max *= 2;
                    Lists = (char *)realloc(Lists, Max);
                    AssertAlways(Lists);
                }
                Size += sprintf(Lists + Size, "== %s\n%s", Downloads[DownloadIndex], List);
                free(List);
            }
            Lists[Size] = 0;
        }
        else if (DownloadCount)
        {
            char *ListScript = (char *)malloc(256 + strlen(Dir) + DownloadCount*66);
            char *At = ListScript;
            At += sprintf(At, "cd %s/files || exit 1\n"
                              "while read h; do echo \"== $h\"; cat $h || exit 1; done <<'EOF'\n", Dir);
            for (u32 DownloadIndex = 0; DownloadIndex < DownloadCount; ++DownloadIndex)
            {
                At += sprintf(At, "%s\n", Downloads[DownloadIndex]);
            }
            At += sprintf(At, "EOF\n");
            if (RunProcess(SshCommand, ListScript, 0, &Lists, 0, false) != 0)
            {
                fprintf(stderr, "%s: can't read the chunk lists\n", Remote);
                Lists = 0;
            }
            free(ListScript);
        }
        // NOTE: Recipe i is file Downloads[i]'s chunks: RecipeChunks[RecipeFirst[i]] on, RecipeCount[i] of them.
        u32 *RecipeFirst = (u32 *)calloc(DownloadCount + 1, sizeof(u32));
        u32 *RecipeCount = (u32 *)calloc(DownloadCount + 1, sizeof(u32));
        bool32 *RecipeFound = (bool32 *)calloc(DownloadCount + 1, sizeof(bool32));
        chunk_list RecipeChunks = {};
        {
            s32 Current = -1;
            for (char *Line = Lists ? strtok(Lists, "\n") : 0; Line; Line = strtok(0, "\n"))
            {
                if (!strncmp(Line, "== ", 3))
                {
                    char *Key = Line + 3;
                    char **Found = (char **)bsearch(&Key, Downloads, DownloadCount, sizeof(char *), CompareStrings);
                    Current = Found ? (s32)(Found - Downloads) : -1;
                    if (Current >= 0)
                    {
                        RecipeFirst[Current] = RecipeChunks.Count;
                        RecipeFound[Current] = true;
                    }
                }
                else if (Current >= 0 && IsHash(Line))
                {
                    chunk Chunk = {};
                    memcpy(Chunk.Hash, Line, 64);
                    AddChunk(&RecipeChunks, Chunk);
                    ++RecipeCount[Current];
                }
            }
        }

        // --- chunks on hand: those of this checkout's old copies (Source = index into OldCopies) ---
        u32 OldCopyCount = 0;
        char **OldCopies = (char **)malloc((ChangeCount + 1)*sizeof(char *));
        chunk_list HaveChunks = {};
        for (u32 ChangeIndex = 0; ChangeIndex < ChangeCount && DownloadCount; ++ChangeIndex)
        {
            change *Change = Changes + ChangeIndex;
            if (Change->OldCopy && !Change->Failed && ChunkFile(Algorithm, Change->OldCopy, OldCopyCount, &HaveChunks, 0))
            {
                OldCopies[OldCopyCount++] = Change->OldCopy;
            }
        }
        if (HaveChunks.Count)
        {
            qsort(HaveChunks.Chunks, HaveChunks.Count, sizeof(chunk), CompareChunks);
        }

        // --- over ssh the chunks not on hand come as one tar stream (a folder remote is read directly) ---
        chunk_list TarChunks = {};
        char TarPath[MAX_PATH] = {};
        snprintf(TarPath, sizeof(TarPath), "%s/pull.tar", TempDir);
        if (!Local)
        {
            u32 WantCount = 0;
            char **Want = (char **)malloc((RecipeChunks.Count + 1)*sizeof(char *));
            for (u32 ChunkIndex = 0; ChunkIndex < RecipeChunks.Count; ++ChunkIndex)
            {
                if (!FindChunk(&HaveChunks, RecipeChunks.Chunks[ChunkIndex].Hash))
                {
                    Want[WantCount++] = RecipeChunks.Chunks[ChunkIndex].Hash;
                }
            }
            WantCount = SortUnique(Want, WantCount);
            if (WantCount)
            {
                char *WantScript = (char *)malloc(256 + strlen(Dir) + WantCount*66);
                char *At = WantScript;
                At += sprintf(At, "cd %s/chunks || exit 1\ntar cf - -T - <<'EOF'\n", Dir);
                for (u32 WantIndex = 0; WantIndex < WantCount; ++WantIndex)
                {
                    At += sprintf(At, "%s\n", Want[WantIndex]);
                }
                At += sprintf(At, "EOF\n");
                if (RunProcess(SshCommand, WantScript, 0, 0, TarPath, false) != 0)
                {
                    fprintf(stderr, "%s: fetching chunks failed\n", Remote);
                }
                free(WantScript);

                // NOTE: Index the tar: a 512-byte header per entry (name, octal size), data padded to 512.
                FILE *Tar = OpenFile(TarPath, L"rb");
                u64 At512 = 0;
                u8 Header[512] = {};
                while (Tar && ReadAt(Tar, At512, Header, sizeof(Header)) && Header[0])
                {
                    chunk Chunk = {};
                    u64 Size = _strtoui64((char *)Header + 124, 0, 8);
                    if (strlen((char *)Header) == 64 && (Header[156] == '0' || Header[156] == 0) && Size <= CHUNK_MAX)
                    {
                        memcpy(Chunk.Hash, Header, 64);
                        Chunk.Offset = At512 + 512;
                        Chunk.Size = (u32)Size;
                        AddChunk(&TarChunks, Chunk);
                    }
                    At512 += 512 + ((Size + 511) & ~511ULL);
                }
                if (Tar)
                {
                    fclose(Tar);
                }
                if (TarChunks.Count)
                {
                    qsort(TarChunks.Chunks, TarChunks.Count, sizeof(chunk), CompareChunks);
                }
            }
            free(Want);
        }

        // --- each file is rebuilt from its chunks next to its path and checked against its hash ---
        FILE *Tar = Local ? 0 : OpenFile(TarPath, L"rb");
        FILE **OldFiles = (FILE **)calloc(OldCopyCount + 1, sizeof(FILE *));
        u8 *Buffer = (u8 *)malloc(CHUNK_MAX);
        u32 ReusedCount = 0;
        u32 FetchedCount = 0;
        u64 FetchedBytes = 0;
        for (u32 ChangeIndex = 0; ChangeIndex < ChangeCount; ++ChangeIndex)
        {
            change *Change = Changes + ChangeIndex;
            bool32 Download = ((Change->Kind == Change_Pull && Change->Server) || Change->Kind == Change_Restored ||
                               Change->Kind == Change_Conflict) && !Change->Failed;
            if (!Download)
            {
                continue;
            }
            char *Key = Change->Server->Hash;
            char **Found = (char **)bsearch(&Key, Downloads, DownloadCount, sizeof(char *), CompareStrings);
            u32 Recipe = Found ? (u32)(Found - Downloads) : 0;
            if (!Found || !RecipeFound[Recipe])
            {
                fprintf(stderr, "%s: no chunk list on the remote\n", Change->Path);
                Change->Failed = true;
                continue;
            }
            char FullPath[MAX_PATH] = {};
            char TempPath[MAX_PATH] = {};
            snprintf(FullPath, sizeof(FullPath), "%s/%s", Root, Change->Path);
            snprintf(TempPath, sizeof(TempPath), "%s.assets-tmp", FullPath);
            for (char *Slash = strchr(FullPath + strlen(Root) + 1, '/'); Slash; Slash = strchr(Slash + 1, '/'))
            {
                char Folder[MAX_PATH] = {};
                snprintf(Folder, sizeof(Folder), "%.*s", (int)(Slash - FullPath), FullPath);
                MakeFolder(Folder);
            }
            FILE *Out = OpenFile(TempPath, L"wb");
            BCRYPT_HASH_HANDLE Hash = 0;
            bool32 Built = Out && (BCryptCreateHash(Algorithm, &Hash, 0, 0, 0, 0, 0) == 0);
            for (u32 Index = 0; Built && Index < RecipeCount[Recipe]; ++Index)
            {
                char *ChunkHash = RecipeChunks.Chunks[RecipeFirst[Recipe] + Index].Hash;
                chunk *FromOld = FindChunk(&HaveChunks, ChunkHash);
                u32 Size = 0;
                if (FromOld)
                {
                    if (!OldFiles[FromOld->Source])
                    {
                        OldFiles[FromOld->Source] = OpenFile(OldCopies[FromOld->Source], L"rb");
                    }
                    Built = OldFiles[FromOld->Source] && ReadAt(OldFiles[FromOld->Source], FromOld->Offset, Buffer, FromOld->Size);
                    Size = FromOld->Size;
                    ++ReusedCount;
                }
                else if (Local)
                {
                    char ChunkPath[MAX_PATH] = {};
                    snprintf(ChunkPath, sizeof(ChunkPath), "%s/chunks/%s", Dir, ChunkHash);
                    FILE *File = OpenFile(ChunkPath, L"rb");
                    Size = File ? (u32)fread(Buffer, 1, CHUNK_MAX, File) : 0;
                    Built = (Size != 0);
                    if (File)
                    {
                        fclose(File);
                    }
                    ++FetchedCount;
                    FetchedBytes += Size;
                }
                else
                {
                    chunk *FromTar = FindChunk(&TarChunks, ChunkHash);
                    Built = FromTar && Tar && ReadAt(Tar, FromTar->Offset, Buffer, FromTar->Size);
                    Size = FromTar ? FromTar->Size : 0;
                    ++FetchedCount;
                    FetchedBytes += Size;
                }
                Built = Built && (fwrite(Buffer, 1, Size, Out) == Size);
                if (Built)
                {
                    BCryptHashData(Hash, Buffer, Size, 0);
                }
            }
            char Got[65] = {};
            if (Hash)
            {
                u8 Digest[32] = {};
                BCryptFinishHash(Hash, Digest, sizeof(Digest), 0);
                BCryptDestroyHash(Hash);
                HexDigest(Digest, Got);
            }
            Built = (Out && fclose(Out) == 0) && Built && !strcmp(Got, Change->Server->Hash);
            if (!Built)
            {
                DeletePath(TempPath);
                fprintf(stderr, "%s: couldn't rebuild it from its chunks\n", FullPath);
                Change->Failed = true;
            }
        }
        for (u32 OldIndex = 0; OldIndex < OldCopyCount; ++OldIndex)
        {
            if (OldFiles[OldIndex])
            {
                fclose(OldFiles[OldIndex]);
            }
        }
        if (Tar)
        {
            fclose(Tar);
        }
        DeletePath(TarPath);

        // --- with the old copies closed, the rebuilt files replace them ---
        for (u32 ChangeIndex = 0; ChangeIndex < ChangeCount; ++ChangeIndex)
        {
            change *Change = Changes + ChangeIndex;
            bool32 Download = ((Change->Kind == Change_Pull && Change->Server) || Change->Kind == Change_Restored ||
                               Change->Kind == Change_Conflict) && !Change->Failed;
            if (Download)
            {
                char FullPath[MAX_PATH] = {};
                char TempPath[MAX_PATH] = {};
                snprintf(FullPath, sizeof(FullPath), "%s/%s", Root, Change->Path);
                snprintf(TempPath, sizeof(TempPath), "%s.assets-tmp", FullPath);
                if (!MovePath(TempPath, FullPath, MOVEFILE_REPLACE_EXISTING))
                {
                    fprintf(stderr, "%s: can't replace it (error %lu)\n", FullPath, GetLastError());
                    DeletePath(TempPath);
                    Change->Failed = true;
                }
            }
            if (Change->Kind == Change_Pull && !Change->Server && Change->OldCopy)
            {
                DeletePath(Change->OldCopy);
            }
        }
        if (DownloadCount)
        {
            printf("pulled: %u chunks fetched (%llu KB), %u reused from this checkout\n", FetchedCount,
                   (FetchedBytes + 1023) >> 10, ReusedCount);
        }

        // --- base.txt becomes the remote's manifest (within the folders), except where pull failed ---
        bool32 RescanFailed = false;
        Here = ScanFolders(Algorithm, Root, Config, &Base, &RescanFailed);
        manifest NewBase = {};
        for (u32 ChangeIndex = 0; ChangeIndex < ChangeCount; ++ChangeIndex)
        {
            change *Change = Changes + ChangeIndex;
            PullFailed = PullFailed || Change->Failed;
            if (Change->Failed && Change->Base)
            {
                AddEntry(&NewBase, *Change->Base);
            }
            else if (!Change->Failed && Change->Server)
            {
                file_entry Entry = *Change->Server;
                file_entry *Current = FindEntry(&Here, Change->Path);
                Entry.WriteTime = (Current && !strcmp(Current->Hash, Entry.Hash)) ? Current->WriteTime : 0;
                AddEntry(&NewBase, Entry);
            }
        }
        u32 BaseSize = 0;
        char *BaseText = FormatManifest(&NewBase, true, &BaseSize);
        PullFailed = !WriteWholeFile(BasePath, BaseText, BaseSize) || RescanFailed || PullFailed;
        free(BaseText);
        Base = NewBase;
        if (PullFailed)
        {
            fprintf(stderr, "pull didn't finish: run it again%s\n", Push ? "; not pushing" : "");
        }
    }

    // SECTION: push: the remote's manifest becomes this checkout's folders (which now have the remote's changes)
    bool32 PushFailed = false;
    if (Push && !PullFailed)
    {
        bool32 UpToDate = (Here.Count == Server.Count);
        for (u32 EntryIndex = 0; UpToDate && EntryIndex < Here.Count; ++EntryIndex)
        {
            UpToDate = !strcmp(Here.Entries[EntryIndex].Path, Server.Entries[EntryIndex].Path) &&
                       !strcmp(Here.Entries[EntryIndex].Hash, Server.Entries[EntryIndex].Hash);
        }
        if (UpToDate)
        {
            printf("the remote is up to date\n");
        }
        else
        {
            // --- the new manifest: this checkout's files, plus the remote's entries outside its folders ---
            manifest Next = {};
            for (u32 EntryIndex = 0; EntryIndex < Here.Count; ++EntryIndex)
            {
                AddEntry(&Next, Here.Entries[EntryIndex]);
            }
            for (u32 EntryIndex = 0; EntryIndex < ServerOut.Count; ++EntryIndex)
            {
                AddEntry(&Next, ServerOut.Entries[EntryIndex]);
            }
            if (Next.Count)
            {
                qsort(Next.Entries, Next.Count, sizeof(file_entry), CompareEntries);
            }

            // --- files whose contents the remote doesn't have, each once, cut into chunks ---
            u32 ServerHashCount = 0;
            char **ServerHashes = (char **)malloc((ServerAll.Count + 1)*sizeof(char *));
            for (u32 EntryIndex = 0; EntryIndex < ServerAll.Count; ++EntryIndex)
            {
                ServerHashes[ServerHashCount++] = ServerAll.Entries[EntryIndex].Hash;
            }
            ServerHashCount = SortUnique(ServerHashes, ServerHashCount);
            u32 UploadCount = 0;
            file_entry **Uploads = (file_entry **)malloc((Here.Count + 1)*sizeof(file_entry *));
            u32 *UploadFirst = (u32 *)calloc(Here.Count + 1, sizeof(u32));
            chunk_list UploadChunks = {};
            for (u32 EntryIndex = 0; EntryIndex < Here.Count && !PushFailed; ++EntryIndex)
            {
                file_entry *Entry = Here.Entries + EntryIndex;
                bool32 OnServer = Contains(ServerHashes, ServerHashCount, Entry->Hash);
                for (u32 UploadIndex = 0; !OnServer && UploadIndex < UploadCount; ++UploadIndex)
                {
                    OnServer = !strcmp(Uploads[UploadIndex]->Hash, Entry->Hash);
                }
                if (OnServer)
                {
                    continue;
                }
                char FullPath[MAX_PATH] = {};
                char FileHash[65] = {};
                snprintf(FullPath, sizeof(FullPath), "%s/%s", Root, Entry->Path);
                UploadFirst[UploadCount] = UploadChunks.Count;
                if (!ChunkFile(Algorithm, FullPath, UploadCount, &UploadChunks, FileHash))
                {
                    fprintf(stderr, "%s: can't read it\n", FullPath);
                    PushFailed = true;
                }
                else if (strcmp(FileHash, Entry->Hash) != 0)
                {
                    fprintf(stderr, "%s: changed while pushing; push again\n", FullPath);
                    PushFailed = true;
                }
                Uploads[UploadCount++] = Entry;
            }
            UploadFirst[UploadCount] = UploadChunks.Count;
            u32 DeletedCount = 0;
            for (u32 EntryIndex = 0; EntryIndex < Server.Count; ++EntryIndex)
            {
                DeletedCount += FindEntry(&Here, Server.Entries[EntryIndex].Path) ? 0 : 1;
            }

            // --- which chunks the remote lacks ---
            chunk_list Unique = {};
            for (u32 ChunkIndex = 0; ChunkIndex < UploadChunks.Count; ++ChunkIndex)
            {
                AddChunk(&Unique, UploadChunks.Chunks[ChunkIndex]);
            }
            if (Unique.Count)
            {
                qsort(Unique.Chunks, Unique.Count, sizeof(chunk), CompareChunks);
                u32 UniqueCount = 0;
                for (u32 ChunkIndex = 0; ChunkIndex < Unique.Count; ++ChunkIndex)
                {
                    if (!UniqueCount || strcmp(Unique.Chunks[UniqueCount - 1].Hash, Unique.Chunks[ChunkIndex].Hash) != 0)
                    {
                        Unique.Chunks[UniqueCount++] = Unique.Chunks[ChunkIndex];
                    }
                }
                Unique.Count = UniqueCount;
            }
            char *Missing = 0;
            if (Unique.Count && !PushFailed && Local)
            {
                Missing = (char *)malloc((size_t)Unique.Count*65 + 1);
                char *At = Missing;
                for (u32 ChunkIndex = 0; ChunkIndex < Unique.Count; ++ChunkIndex)
                {
                    char ChunkPath[MAX_PATH] = {};
                    snprintf(ChunkPath, sizeof(ChunkPath), "%s/chunks/%s", Dir, Unique.Chunks[ChunkIndex].Hash);
                    if (!Exists(ChunkPath))
                    {
                        At += sprintf(At, "%s\n", Unique.Chunks[ChunkIndex].Hash);
                    }
                }
                *At = 0;
            }
            else if (Unique.Count && !PushFailed)
            {
                char *AskScript = (char *)malloc(256 + strlen(Dir) + Unique.Count*66);
                char *At = AskScript;
                At += sprintf(At, "cd %s/chunks 2>/dev/null || { cat; exit 0; }\n"
                                  "while read h; do [ -f $h ] || echo $h; done <<'EOF'\n", Dir);
                for (u32 ChunkIndex = 0; ChunkIndex < Unique.Count; ++ChunkIndex)
                {
                    At += sprintf(At, "%s\n", Unique.Chunks[ChunkIndex].Hash);
                }
                At += sprintf(At, "EOF\n");
                if (RunProcess(SshCommand, AskScript, 0, &Missing, 0, false) != 0)
                {
                    fprintf(stderr, "%s: can't ask which chunks it has\n", Remote);
                    PushFailed = true;
                }
                free(AskScript);
            }

            // --- stage: missing chunks, every changed file's chunk list, the manifest (over ssh: a tar + commit.sh) ---
            char IncomingId[MAX_PATH] = {};
            {
                SYSTEMTIME Now = {};
                GetLocalTime(&Now);
                snprintf(IncomingId, sizeof(IncomingId), "%.16s-%04u%02u%02u-%02u%02u%02u-%lu", Checkout, Now.wYear,
                         Now.wMonth, Now.wDay, Now.wHour, Now.wMinute, Now.wSecond, GetCurrentProcessId());
                for (char *C = IncomingId; *C; ++C)
                {
                    *C = (isalnum((u8)*C) || *C == '-') ? *C : '_';
                }
            }
            char Incoming[MAX_PATH] = {};
            char TarPath[MAX_PATH] = {};
            snprintf(Incoming, sizeof(Incoming), "%s/incoming/%s", Dir, IncomingId);
            snprintf(TarPath, sizeof(TarPath), "%s/push.tar", TempDir);
            FILE *Tar = 0;
            if (!PushFailed && Local)
            {
                char Folder[MAX_PATH] = {};
                MakeFolder(Dir);
                for (u32 Index = 0; Index < 4; ++Index)
                {
                    char *Names[] = {"chunks", "files", "manifests", "incoming"};
                    snprintf(Folder, sizeof(Folder), "%s/%s", Dir, Names[Index]);
                    MakeFolder(Folder);
                }
                MakeFolder(Incoming);
                snprintf(Folder, sizeof(Folder), "%s/chunks", Incoming);
                MakeFolder(Folder);
                snprintf(Folder, sizeof(Folder), "%s/files", Incoming);
                MakeFolder(Folder);
                PushFailed = !Exists(Folder);
                if (PushFailed)
                {
                    fprintf(stderr, "%s: can't write there\n", Dir);
                }
            }
            else if (!PushFailed)
            {
                Tar = OpenFile(TarPath, L"wb");
                PushFailed = !Tar;
                if (PushFailed)
                {
                    fprintf(stderr, "%s: can't write it\n", TarPath);
                }
            }
            u32 SentChunks = 0;
            u64 SentBytes = 0;
            if (!PushFailed)
            {
                FILE **Sources = (FILE **)calloc(UploadCount + 1, sizeof(FILE *));
                u8 *Buffer = (u8 *)malloc(CHUNK_MAX);
                for (char *Line = Missing ? strtok(Missing, "\n") : 0; Line && !PushFailed; Line = strtok(0, "\n"))
                {
                    chunk *Chunk = (strlen(Line) == 64) ? FindChunk(&Unique, Line) : 0;
                    if (!Chunk)
                    {
                        continue;
                    }
                    char FullPath[MAX_PATH] = {};
                    snprintf(FullPath, sizeof(FullPath), "%s/%s", Root, Uploads[Chunk->Source]->Path);
                    if (!Sources[Chunk->Source])
                    {
                        Sources[Chunk->Source] = OpenFile(FullPath, L"rb");
                    }
                    u8 Digest[32] = {};
                    char Got[65] = {};
                    bool32 Read = Sources[Chunk->Source] && ReadAt(Sources[Chunk->Source], Chunk->Offset, Buffer, Chunk->Size);
                    BCryptHash(Algorithm, 0, 0, Buffer, Chunk->Size, Digest, sizeof(Digest));
                    HexDigest(Digest, Got);
                    if (!Read || strcmp(Got, Chunk->Hash) != 0)
                    {
                        fprintf(stderr, "%s: changed while pushing; push again\n", FullPath);
                        PushFailed = true;
                        continue;
                    }
                    if (Local)
                    {
                        char ChunkPath[MAX_PATH] = {};
                        snprintf(ChunkPath, sizeof(ChunkPath), "%s/chunks/%s", Incoming, Chunk->Hash);
                        FILE *File = OpenFile(ChunkPath, L"wb");
                        bool32 Written = File && (fwrite(Buffer, 1, Chunk->Size, File) == Chunk->Size);
                        Written = File && (fclose(File) == 0) && Written;
                        if (!Written)
                        {
                            fprintf(stderr, "%s: can't write it\n", ChunkPath);
                            PushFailed = true;
                        }
                    }
                    else
                    {
                        char Name[MAX_PATH] = {};
                        snprintf(Name, sizeof(Name), "chunks/%s", Chunk->Hash);
                        WriteTarEntry(Tar, Name, Buffer, Chunk->Size);
                    }
                    ++SentChunks;
                    SentBytes += Chunk->Size;
                }
                for (u32 UploadIndex = 0; UploadIndex < UploadCount; ++UploadIndex)
                {
                    if (Sources[UploadIndex])
                    {
                        fclose(Sources[UploadIndex]);
                    }
                    u32 First = UploadFirst[UploadIndex];
                    u32 Count = UploadFirst[UploadIndex + 1] - First;
                    char *Recipe = (char *)malloc((size_t)Count*65 + 1);
                    char *At = Recipe;
                    for (u32 Index = 0; Index < Count; ++Index)
                    {
                        At += sprintf(At, "%s\n", UploadChunks.Chunks[First + Index].Hash);
                    }
                    char Name[MAX_PATH] = {};
                    snprintf(Name, sizeof(Name), "files/%s", Uploads[UploadIndex]->Hash);
                    if (Local)
                    {
                        char ListPath[MAX_PATH] = {};
                        snprintf(ListPath, sizeof(ListPath), "%s/%s", Incoming, Name);
                        PushFailed = !WriteWholeFile(ListPath, Recipe, (u32)(At - Recipe)) || PushFailed;
                    }
                    else if (Tar)
                    {
                        WriteTarEntry(Tar, Name, (u8 *)Recipe, (u64)(At - Recipe));
                    }
                    free(Recipe);
                }
                u32 ManifestSize = 0;
                char *ManifestText = FormatManifest(&Next, false, &ManifestSize);
                if (Local)
                {
                    char ManifestPath[MAX_PATH] = {};
                    snprintf(ManifestPath, sizeof(ManifestPath), "%s/manifest.txt", Incoming);
                    PushFailed = !WriteWholeFile(ManifestPath, ManifestText, ManifestSize) || PushFailed;
                }
                else if (Tar)
                {
                    WriteTarEntry(Tar, "manifest.txt", (u8 *)ManifestText, ManifestSize);

                    // NOTE: Runs on the remote: every chunk must match its name and every file its chunks
                    //       before the manifest is replaced; anything else leaves the remote as it was.
                    snprintf(Script, sizeof(Script),
                             "export LC_ALL=C\n"
                             "cd %s || exit 1\n"
                             "in=incoming/%s\n"
                             "fail() { echo \"$1\" >&2; rm -rf $in; exit 1; }\n"
                             "mkdir -p chunks files manifests\n"
                             "if [ -d $in/chunks ]; then\n"
                             "  (cd $in/chunks && ls | xargs -r sha256sum) | awk '$1 != $2 { print \"bad chunk \" $2; bad = 1 } END { exit bad }' || fail 'chunks arrived damaged; push again'\n"
                             "  find $in/chunks -type f -exec mv -f -t chunks {} +\n"
                             "fi\n"
                             "for r in $in/files/*; do\n"
                             "  [ -f \"$r\" ] || continue\n"
                             "  got=$(sed 's|^|chunks/|' $r | xargs -r cat | sha256sum | cut -c1-64)\n"
                             "  [ \"$got\" = \"${r##*/}\" ] || fail 'a file changed while it was pushed; push again'\n"
                             "  mv -f $r files/\n"
                             "done\n"
                             "cur=none; [ -f manifest.txt ] && cur=$(sha256sum < manifest.txt | cut -c1-64)\n"
                             "[ \"$cur\" = %s ] || fail 'the remote changed during this push; push again'\n"
                             "t=$(date -u +%%Y%%m%%dT%%H%%M%%S%%NZ)\n"
                             "if [ -f manifest.txt ]; then mv manifest.txt manifests/$t.txt && touch manifests/$t.txt || fail 'can not keep the old manifest'; fi\n"
                             "mv $in/manifest.txt manifest.txt || fail 'can not replace the manifest'\n"
                             "echo '" STORE_FORMAT "' > format\n"
                             "rm -rf $in\n"
                             "find manifests -name '*.txt' -mtime +%d -delete\n"
                             "find incoming -mindepth 1 -maxdepth 1 -mmin +1440 -exec rm -rf {} + 2>/dev/null\n"
                             "cat manifest.txt manifests/*.txt 2>/dev/null | cut -d' ' -f1 | sort -u > .keepfiles\n"
                             "ls files | sort | comm -23 - .keepfiles | sed 's|^|files/|' | xargs -r rm -f\n"
                             "(cd files && xargs -r cat < ../.keepfiles 2>/dev/null) | sort -u > .keepchunks\n"
                             "[ -s .keepchunks ] || [ ! -s manifest.txt ] || { rm -f .keepfiles .keepchunks; exit 0; }\n"
                             "ls chunks | sort | comm -23 - .keepchunks | sed 's|^|chunks/|' | xargs -r rm -f\n"
                             "rm -f .keepfiles .keepchunks\n", Dir, IncomingId, ServerHash, TRASH_DAYS);
                    WriteTarEntry(Tar, "commit.sh", (u8 *)Script, strlen(Script));
                    u8 End[1024] = {};
                    fwrite(End, 1, sizeof(End), Tar);
                }
                free(ManifestText);
                free(Buffer);
                free(Sources);
            }
            if (Tar)
            {
                PushFailed = (ferror(Tar) != 0) || PushFailed;
                PushFailed = (fclose(Tar) != 0) || PushFailed;
            }

            // --- commit: over ssh one stream, unpacked into incoming/ID and its commit.sh run ---
            if (!PushFailed && !Local)
            {
                char UploadCommand[2048] = {};
                snprintf(UploadCommand, sizeof(UploadCommand),
                         "%s -o Compression=yes %s mkdir -p %s && tar xf - -C %s && bash %s/commit.sh",
                         Ssh, Host, Incoming, Incoming, Incoming);
                PushFailed = (RunProcess(UploadCommand, 0, TarPath, 0, 0, false) != 0);
                if (PushFailed)
                {
                    fprintf(stderr, "%s: the push failed; the remote is unchanged\n", Remote);
                }
            }
            DeletePath(TarPath);

            // --- commit to a folder remote: the same steps as commit.sh, done here ---
            if (Local)
            {
                // NOTE: The chunks were checked against their hashes as they were written, and each file
                //       against its own while it was cut, so they go straight in.
                char From[MAX_PATH] = {};
                char To[MAX_PATH] = {};
                for (u32 Pass = 0; Pass < 2 && !PushFailed; ++Pass)
                {
                    char *Kind = Pass ? "files" : "chunks";
                    snprintf(From, sizeof(From), "%s/%s", Incoming, Kind);
                    u32 Count = 0;
                    folder_entry *Entries = ListFolder(From, &Count);
                    for (u32 Index = 0; Index < Count && !PushFailed; ++Index)
                    {
                        snprintf(From, sizeof(From), "%s/%s/%s", Incoming, Kind, Entries[Index].Name);
                        snprintf(To, sizeof(To), "%s/%s/%s", Dir, Kind, Entries[Index].Name);
                        PushFailed = !MovePath(From, To, MOVEFILE_REPLACE_EXISTING|MOVEFILE_COPY_ALLOWED);
                    }
                }
                char Current[MAX_PATH] = {};
                char CurrentHash[65] = "none";
                snprintf(Current, sizeof(Current), "%s/manifest.txt", Dir);
                if (!PushFailed && Exists(Current) && !HashFile(Algorithm, Current, CurrentHash))
                {
                    PushFailed = true;
                }
                if (!PushFailed && strcmp(CurrentHash, ServerHash) != 0)
                {
                    fprintf(stderr, "the remote changed during this push; push again\n");
                    PushFailed = true;
                }
                if (!PushFailed && Exists(Current))
                {
                    SYSTEMTIME Now = {};
                    GetSystemTime(&Now);
                    snprintf(To, sizeof(To), "%s/manifests/%04u%02u%02uT%02u%02u%02u%03uZ-%lu.txt", Dir, Now.wYear,
                             Now.wMonth, Now.wDay, Now.wHour, Now.wMinute, Now.wSecond, Now.wMilliseconds,
                             GetCurrentProcessId());
                    PushFailed = !MovePath(Current, To, MOVEFILE_COPY_ALLOWED);
                    // NOTE: Its time becomes the time it stopped being current: the 30 days count from here.
                    wchar_t WideTo[MAX_PATH] = {};
                    Widen(To, WideTo, MAX_PATH);
                    HANDLE History = CreateFileW(WideTo, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
                    if (History != INVALID_HANDLE_VALUE)
                    {
                        FILETIME Touch = {};
                        GetSystemTimeAsFileTime(&Touch);
                        SetFileTime(History, 0, 0, &Touch);
                        CloseHandle(History);
                    }
                }
                snprintf(From, sizeof(From), "%s/manifest.txt", Incoming);
                if (!PushFailed)
                {
                    PushFailed = !MovePath(From, Current, MOVEFILE_REPLACE_EXISTING|MOVEFILE_COPY_ALLOWED);
                }
                if (!PushFailed)
                {
                    char FormatPath[MAX_PATH] = {};
                    snprintf(FormatPath, sizeof(FormatPath), "%s/format", Dir);
                    WriteWholeFile(FormatPath, STORE_FORMAT "\n", (u32)strlen(STORE_FORMAT "\n"));
                }
                DeleteTree(Incoming);
                if (PushFailed)
                {
                    fprintf(stderr, "%s: the push failed; the remote's manifest is unchanged\n", Remote);
                }

                // --- the trash: history older than 30 days, then file lists and chunks no manifest left needs ---
                if (!PushFailed)
                {
                    u64 Now = NowFileTime();
                    u64 Days = 24ULL*3600*10000000;
                    u32 KeepCount = 0;
                    u32 KeepMax = 0;
                    char **Keep = 0;
                    for (u32 EntryIndex = 0; EntryIndex < Next.Count; ++EntryIndex)
                    {
                        AddString(&Keep, &KeepCount, &KeepMax, Next.Entries[EntryIndex].Hash);
                    }
                    char Folder[MAX_PATH] = {};
                    snprintf(Folder, sizeof(Folder), "%s/manifests", Dir);
                    u32 HistoryCount = 0;
                    folder_entry *Histories = ListFolder(Folder, &HistoryCount);
                    for (u32 Index = 0; Index < HistoryCount; ++Index)
                    {
                        char Path[MAX_PATH] = {};
                        snprintf(Path, sizeof(Path), "%s/%s", Folder, Histories[Index].Name);
                        if (Now - Histories[Index].WriteTime > TRASH_DAYS*Days)
                        {
                            DeletePath(Path);
                            continue;
                        }
                        char *Text = ReadText(Path, 0);
                        for (char *Line = Text ? strtok(Text, "\n") : 0; Line; Line = strtok(0, "\n"))
                        {
                            if (strlen(Line) > 64)
                            {
                                Line[64] = 0;
                                AddString(&Keep, &KeepCount, &KeepMax, Line);
                            }
                        }
                    }
                    KeepCount = SortUnique(Keep, KeepCount);
                    u32 KeepChunkCount = 0;
                    u32 KeepChunkMax = 0;
                    char **KeepChunks = 0;
                    snprintf(Folder, sizeof(Folder), "%s/files", Dir);
                    u32 ListCount = 0;
                    folder_entry *Lists = ListFolder(Folder, &ListCount);
                    for (u32 Index = 0; Index < ListCount; ++Index)
                    {
                        char Path[MAX_PATH] = {};
                        snprintf(Path, sizeof(Path), "%s/%s", Folder, Lists[Index].Name);
                        if (!Contains(Keep, KeepCount, Lists[Index].Name))
                        {
                            DeletePath(Path);
                            continue;
                        }
                        char *Text = ReadText(Path, 0);
                        for (char *Line = Text ? strtok(Text, "\n") : 0; Line; Line = strtok(0, "\n"))
                        {
                            AddString(&KeepChunks, &KeepChunkCount, &KeepChunkMax, Line);
                        }
                    }
                    KeepChunkCount = SortUnique(KeepChunks, KeepChunkCount);
                    // NOTE: No chunk to keep with files in the manifest means something went wrong: keep them all.
                    if (KeepChunkCount || !Next.Count)
                    {
                        snprintf(Folder, sizeof(Folder), "%s/chunks", Dir);
                        u32 ChunkCount = 0;
                        folder_entry *Chunks = ListFolder(Folder, &ChunkCount);
                        for (u32 Index = 0; Index < ChunkCount; ++Index)
                        {
                            if (!Contains(KeepChunks, KeepChunkCount, Chunks[Index].Name))
                            {
                                char Path[MAX_PATH] = {};
                                snprintf(Path, sizeof(Path), "%s/%s", Folder, Chunks[Index].Name);
                                DeletePath(Path);
                            }
                        }
                    }
                    snprintf(Folder, sizeof(Folder), "%s/incoming", Dir);
                    u32 IncomingCount = 0;
                    folder_entry *Stale = ListFolder(Folder, &IncomingCount);
                    for (u32 Index = 0; Index < IncomingCount; ++Index)
                    {
                        if (Stale[Index].IsFolder && Now - Stale[Index].WriteTime > Days)
                        {
                            char Path[MAX_PATH] = {};
                            snprintf(Path, sizeof(Path), "%s/%s", Folder, Stale[Index].Name);
                            DeleteTree(Path);
                        }
                    }
                }
            }

            if (!PushFailed)
            {
                u32 BaseSize = 0;
                char *BaseText = FormatManifest(&Here, true, &BaseSize);
                PushFailed = !WriteWholeFile(BasePath, BaseText, BaseSize);
                free(BaseText);
                printf("pushed: %u files changed, %u chunks sent (%llu KB), %u files deleted (kept on the remote for %d days)\n",
                       UploadCount, SentChunks, (SentBytes + 1023) >> 10, DeletedCount, TRASH_DAYS);
            }
            free(Missing);
            free(Unique.Chunks);
            free(UploadChunks.Chunks);
            free(UploadFirst);
            free(Uploads);
            free(ServerHashes);
        }
    }

    // SECTION: conflict copies wait for the user, however long that takes
    u32 AttentionCount = 0;
    for (u32 EntryIndex = 0; EntryIndex < Here.Count; ++EntryIndex)
    {
        if (strstr(Here.Entries[EntryIndex].Path, ".conflict-"))
        {
            if (!AttentionCount++)
            {
                printf("needs your attention (keep one, delete the other):\n");
            }
            printf("    %s\n", Here.Entries[EntryIndex].Path);
        }
    }

    return (PullFailed || PushFailed) ? 1 : 0;
}
