# casi remote-store format

This document is the compatibility contract for a Git remote written by the
casi 0.1 release series. It describes the data below `refs/heads/casi/`, not
the local,
disposable cache or the user's Claude Code directory. A conforming client may
read an older supported format, but must reject a format it does not recognise
and must not rewrite it as a different format.

The store is deliberately a normal bare Git repository. casi writes blobs,
trees and commits directly; it does not use a Git worktree or an index file.
Git object IDs, not filenames, identify content.

## Refs

Each machine owns one ref:

```
refs/heads/casi/<machine>
```

`<machine>` is one valid Git ref component, must not contain `/`, and cannot be
`config`. A machine only writes its own ref. The commit tree is a complete
snapshot of that machine's sessions and auxiliary files, rather than a delta.

The one shared ref is:

```
refs/heads/casi/config
```

It holds the canonical named-root namespace and shared exclusion list. Writers
parent a configuration update on the fetched configuration ref and use a normal
fast-forward push; a non-fast-forward is retried after fetching and replaying
the mutation. It is not a machine snapshot.

Consumers that merge several machine refs must use casi's prefix-conflict
rules. This document specifies the storage representation, not a last-writer-
wins merge rule.

## Machine snapshot, format 1

An unencrypted snapshot has this logical tree. `<provider>` is `claude-code`
in format 1.

```
casi.json
sessions/
  <provider>/<project-id>/<session-id>/
    meta.json
    chunks/000000
           000001
           ...
    subagents/<filename>
projects/
  <provider>/<project-id>/
    meta.json
    memory/<filename>
```

`casi.json` is the JSON object `{"format":1}` followed by a newline. Its
presence identifies the machine-tree format, independently of the similarly
named configuration-ref file described below.

`<project-id>` is the hexadecimal Git blob object ID obtained by hashing the
canonical project path's bytes. The canonical, human-readable path travels in
metadata so it is not reconstructed from this identifier. `<session-id>` is
the provider's session identifier. Chunk names are zero-padded decimal indices
starting at zero and are ordered lexicographically.

A session's `meta.json` is one JSON object followed by a newline:

```json
{
  "sessionId": "<session-id>",
  "projectId": "<project-id>",
  "projectPath": "casi://<root>/<path>",
  "chunks": 2,
  "chunkOids": ["<git-object-id>", "<git-object-id>"],
  "bytes": 1234
}
```

`bytes` is the normalized transcript length. A chunk is a normalized JSONL
byte range: it reaches at least 1 MiB and then ends immediately after the next
newline. The only short chunk is the final chunk; a single long line may exceed
1 MiB. The metadata lists the chunk object IDs in the same order as the
`chunks/` entries.

Project metadata exists when that project has memory files:

```json
{"projectId":"<project-id>","projectPath":"casi://<root>/<path>"}
```

Transcript chunks, subagent files and memory files contain normalized text.
Before materializing them on another machine, casi translates named-root paths
back to that machine's local layout. Consequently, filesystem bytes on two
machines with different root mappings need not be identical even though the
normalized Git blobs are.

## Shared configuration, format 1

In an unencrypted store, the configuration ref has one blob named `casi.json`:

```json
{"format":1,"roots":["src"],"sync":{"exclude":["casi://src/private"]}}
```

The arrays are sorted lexicographically when written. `roots` contains shared
root names, never the implicit `~` root. `sync.exclude` contains canonical
project paths. The configuration format number is scoped to this ref; it is
not the machine-snapshot format number.

## Encrypted stores

Encryption is selected when an otherwise empty remote is initialized with
`casi init --encrypt`. It is a property of the remote: casi does not convert a
remote that already contains a machine or configuration ref. A joining machine
must have received the same private local key through a separate channel.

### Key and blobs

The local keyfile is exactly 32 random bytes and must be mode `0600`. It is
never stored in the remote. From it, casi derives distinct keys for content,
nonces and paths.

Every ordinary blob in an encrypted machine snapshot is deterministically
encrypted with XChaCha20-Poly1305. Its binary envelope is:

```
"CASIENC1" || nonce[24] || ciphertext-and-tag
```

The nonce is derived from the normalized plaintext with the nonce subkey.
Identical normalized content therefore produces an identical Git blob, which
preserves chunk deduplication and the append-prefix rule. The envelope adds 48
bytes. Authentication failure is fatal; consumers must not materialize partial
data.

Every logical component of a machine-tree path is replaced with the lowercase
hex HMAC-SHA256 of that component under the path subkey. This includes fixed
components such as `sessions`, `meta.json` and chunk indices as well as project
and session identifiers. A keyless clone therefore cannot enumerate readable
project names, session IDs, provider names or tree vocabulary.

After decrypting metadata, a reader verifies each opaque project and session
directory against its logical identifier and verifies every `chunkOids` item
against the object referenced by the corresponding tree entry. It also verifies
the filename and scope carried by an auxiliary file before materializing it.

An encrypted auxiliary plaintext is wrapped before ordinary blob encryption:

```
"CASIAS1" || uint32le(scope-length) || uint32le(name-length) ||
scope || filename || normalized-content
```

The UTF-8 `scope` is `"<kind>\n<project-id>\n<session-id>"`, where the
session ID is empty for project memory. This wrapper lets casi recover opaque
leaf names and prevents a valid encrypted auxiliary blob being transplanted to
another project, session or asset kind.

### Encrypted shared configuration, format 2

The `refs/heads/casi/config` tree deliberately has a cleartext header so a
joining client can determine whether it needs a key:

```json
{"format":2,"crypto":{"mode":"convergent-v1","keyId":"<hex>"}}
```

`keyId` is a non-secret, deterministic keyed identifier used only to detect a
wrong key. It is not the key and does not reveal roots or exclusions. The
configuration payload is the same normalized JSON object as the unencrypted
configuration format, encrypted in a second blob. That blob's tree name is the
HMAC path component of the literal `config` and it is authenticated with that
opaque name as associated data.

All machine-tree metadata, chunks and auxiliary contents remain ordinary
encrypted blobs; the cleartext header is the sole intentional exception.

## Compatibility rules

- Format 1 machine snapshots and format 1 shared configurations are stable.
  Writers may add JSON fields, but readers must preserve the semantics of the
  required fields documented here.
- Format 2 is reserved for the encrypted shared-configuration header. It does
  not make an unencrypted machine snapshot format 2.
- A change to tree layout, required metadata, chunking, encrypted envelope,
  path encoding or authenticated bindings requires a new compatible-reader
  path and a new format version; it must never be silently migrated in place.
- Git history is transport history, not an access-control boundary. Encryption
  protects names and contents from a keyless reader; it does not prevent a
  remote that can rewrite refs from rolling a client back to an older valid
  snapshot.

Do not edit a casi store by hand. Use a normal Git clone or `git cat-file` for
inspection, but use casi to write it so its ref ownership, metadata checks and
conflict rules are retained.
