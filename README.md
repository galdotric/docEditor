# docEditor: Collaborative Markdown Editor in C

A **server and command-line clients for editing one Markdown document together**. Clients connect with a real-time signal handshake and then talk to the server over a pair of named pipes (FIFOs). The server gives each client its own thread, checks each user's read or write permission from `roles.txt`, applies edits to a shared document behind a mutex, and keeps a log of every edit. Each successful edit becomes a new document version.

The project started from a provided scaffold: the header interfaces, the Makefile and a starter `markdown.c`. The server, the client and the editing logic were built on top of it.

---

## Contents

1. [Quick start](#1-quick-start)
2. [Usage](#2-usage)
3. [Commands](#3-commands)
4. [Protocol](#4-protocol)
5. [Architecture](#5-architecture)
6. [Versions and positions](#6-versions-and-positions)
7. [Files](#7-files)
8. [Known limitations](#8-known-limitations)

---

## 1. Quick start

```sh
make
./server                      # prints: Server Process ID: <pid>
./client <pid> daniel         # in another terminal
```

Then type commands at the client's `>` prompt:

```
> INSERT 0 Hello world
EDIT daniel INSERT 0 Hello world SUCCESS
SUCCESS
> BOLD 0 5
EDIT daniel BOLD 0 5 SUCCESS
SUCCESS
> HEADING 1 0
EDIT daniel HEADING 1 0 SUCCESS
SUCCESS
> DOC?
SUCCESS
# **Hello** world
```

Type `QUIT` in the server's terminal once every client has disconnected. The document is saved to `doc.md`.

---

## 2. Usage

### Building

| Command | Result |
|---|---|
| `make` | Builds `server` and `client` (gcc, C11). It links the AddressSanitizer runtime but compiles with `-fno-sanitize=all`, so the code itself is not checked |
| `make valgrind` | Rebuilds without sanitizers, for running under Valgrind |
| `make asan` | Same flags as `make` |
| `make clean` | Removes the binaries and `obj/` |

Requires Linux (real-time signals, `mkfifo`) and pthreads.

### Running

| Program | Arguments | Notes |
|---|---|---|
| `server` | none | Prints its PID, then waits for clients. Type `QUIT` on its standard input to save and exit |
| `client` | `<server_pid> <username>` | Connects, prints the user's role and the current document, then reads commands from standard input |

Run both from the folder that holds `roles.txt`. The server creates its pipes and `doc.md` in its working directory.

### Permissions

`roles.txt` has one user per line, then `read` or `write`:

```
ryan read
yao read
daniel write
```

| Role | Can |
|---|---|
| `write` | Every command |
| `read` | `DOC?`, `LOG?`, `PERM?`, `DISCONNECT`; edits are rejected with `Reject UNAUTHORISED` |
| not listed | Nothing: the connection is refused with `Reject UNAUTHORISED` |

The file is read again on every connection, so changes apply to new clients without restarting the server.

---

## 3. Commands

Positions count characters from the start of the document, starting at 0.

### Edits (writers only)

| Command | Effect |
|---|---|
| `INSERT <pos> <text>` | Insert text (no newlines; keep it under 192 characters, see [Known limitations](#8-known-limitations)) |
| `DEL <pos> <len>` | Delete `len` characters |
| `DELETE <version> <pos> <len>` | Delete, but only if `version` is still the current version |
| `NEWLINE <pos>` | Insert a line break |
| `HEADING <level> <pos>` | Insert `#`, `##` or `###` (level 1 to 3), on a new line if `pos` isn't at the start of one |
| `BOLD <start> <end>` | Wrap the range in `**` |
| `ITALIC <start> <end>` | Wrap the range in `*` |
| `CODE <start> <end>` | Wrap the range in `` ` `` |
| `LINK <start> <end> <url>` | Turn the range into `[text](url)` |
| `BLOCKQUOTE <pos>` | Insert `> ` at the start of a line |
| `UNORDERED_LIST <pos>` | Insert `- ` at the start of a line |
| `ORDERED_LIST <pos>` | Insert `N. `, continuing from the number on the line above (1 to 9, then back to 1) |
| `HORIZONTAL_RULE <pos>` | Insert a `---` line |

### Queries and control (everyone)

| Command | Reply |
|---|---|
| `DOC?` | `SUCCESS`, then the document |
| `LOG?` | `SUCCESS`, every logged edit, then `END` |
| `PERM?` | The user's role (`read` or `write`); see [Known limitations](#8-known-limitations) |
| `DISCONNECT` | Closes the connection |

### Replies to edits

Every edit command, accepted or not, is logged and echoed as `EDIT <user> <command> <result>`, followed by the result on its own line:

| Result | Meaning |
|---|---|
| `SUCCESS` | Applied; the document is now one version newer |
| `Reject UNAUTHORISED` | The user has the `read` role |
| `Reject INVALID_POSITION` | A position is past the end of the document, or a range is empty or backwards |
| `Reject OUTDATED_VERSION` | `DELETE` named a version that is no longer current |
| `Reject INVALID_COMMAND_FORMAT` | Wrong or missing arguments (not logged) |
| `Reject INVALID_COMMAND` | Unknown command (not logged) |

The server also prints every logged edit on its own standard output.

---

## 4. Protocol

### Connecting

| Step | Who | What |
|---|---|---|
| 1 | Client | Sends `SIGRTMIN` to the server's PID |
| 2 | Server | Reads the client's PID from the signal, creates `FIFO_C2S_<pid>` and `FIFO_S2C_<pid>`, starts a thread for the client and sends it `SIGRTMIN + 1` |
| 3 | Client | Opens both pipes and writes its username |
| 4 | Server | Looks up the user in `roles.txt`. If not found, writes `Reject UNAUTHORISED` and closes |
| 5 | Server | Writes the role, the current version, the document's length in bytes, then the document itself |
| 6 | Both | Exchange line-based commands and replies until `DISCONNECT` |

### Messages

Everything is plain text, one message per line. Commands go client → server on `FIFO_C2S_<pid>`; replies go server → client on `FIFO_S2C_<pid>`. The pipes are deleted when the client disconnects.

---

## 5. Architecture

| Component | File | Does |
|---|---|---|
| Server main loop | `source/server.c` | Polls standard input for `QUIT` every 100 ms and starts a client thread whenever a connection signal has arrived |
| Client thread | `source/server.c` | Runs the handshake, reads commands, calls the document functions, logs and replies |
| Document | `source/markdown.c`, `libs/document.h` | The text as a linked list of chunks, every edit command, versions and deletions |
| Client | `source/client.c` | Connects, then sends each typed command and prints the reply |

| Shared state | Protected by |
|---|---|
| The document | `document_access_mutex`, held for the whole of each command |
| Number of connected clients | `client_counter_mutex` |
| The edit log | Nothing: it is written after the document mutex is released (see [Known limitations](#8-known-limitations)) |

`QUIT` is refused while any client is connected, so the document is never freed while a thread is still using it.

---

## 6. Versions and positions

- The document keeps two copies of its text: the **live text**, which edits change, and the **committed text**, which `DOC?`, new clients and `doc.md` see.
- After every successful edit, the server calls `markdown_increment_version()`. It copies the live text to the committed text and adds 1 to the version.
- So each successful edit is its own version, and readers always see the latest one.
- Only `DELETE` takes a version from the client. Every other command is applied to the current version, so it can't be rejected as outdated.
- Within a version, a position that falls inside text deleted earlier in that version is moved to the start of the deletion. With one edit per version this rarely comes up, but the document functions support it.

---

## 7. Files

| File | Role |
|---|---|
| `source/server.c` | Server: handshake, per-client threads, command dispatch, edit log, shutdown |
| `source/client.c` | Command-line client |
| `source/markdown.c` | The document and every edit command |
| `libs/markdown.h` | Document functions and their return codes |
| `libs/document.h` | The `document` and `chunk` types |
| `roles.txt` | Users and their roles |
| `Makefile` | Builds `server` and `client` |

---

## 8. Known limitations

- **A long insert crashes the server.** `INSERT` copies its text into a 192-byte buffer with no length limit, while a command line can be 255 characters. Inserting 230 characters ends the server with `*** stack smashing detected ***`, so any user with the `write` role can stop it.
- **The edit log isn't locked.** Two clients editing at the same moment can both write to the log at once and corrupt it, because it is updated after the document mutex is released.
- **`PERM?` hangs the bundled client.** The server replies with just the role (`write`) and no `SUCCESS` line, but the client keeps reading until it sees `SUCCESS` or `Reject`, so it waits forever. Other clients and the server are unaffected.
- **Edits are not pushed to other clients.** The `EDIT` line goes only to the client that sent the command. Other clients see the change with `DOC?`.
- **Connections that arrive at the same moment can be lost.** The server remembers only one pending connection signal at a time, so if two clients signal before the main loop handles the first, one of them waits forever.
- **The document starts empty every time.** `doc.md` is written on `QUIT` but never read back.
- **Fixed-size limits.** A command line longer than 255 characters is split, and the rest is read as a second command. URLs are cut at 127 characters, and the edit log stops recording after 1,024 entries.
- **`DELETED_POSITION` is never produced.** The server can report it, but the document functions never return it; positions in deleted text are moved instead (see [§6](#6-versions-and-positions)).
