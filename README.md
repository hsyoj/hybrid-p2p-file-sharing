# PeerShare: A Hybrid P2P File-Sharing System in C

A hybrid peer-to-peer file-sharing system written in C. A central **index server** handles authentication, peer presence and file discovery over **UDP**, while peers transfer files **directly to each other over TCP**. The server never stores or relays file contents.

The client and server (about 2,000 lines of C in total) are written against the raw socket API with no networking libraries, and build on both Linux/macOS (POSIX) and Windows (Winsock).

## Features

- **User authentication** against a credentials file on the server
- **Presence tracking** with periodic UDP heartbeats from each client
- **File publishing and unpublishing**: peers advertise which files they are willing to share
- **File search and listing**: look up published files and list active peers
- **Direct peer-to-peer downloads** over TCP; the server only tells the requester which peer to contact
- **Multi-threaded design**: the client runs a heartbeat thread and a TCP file server alongside its interactive shell, and the server runs separate heartbeat handling and liveness-checking threads
- **Cross-platform** build via a small portability layer for threads, sockets and timing

## Architecture

```
                    ┌──────────────────────────┐
        UDP         │       Index Server       │         UDP
  ┌───────────────► │  auth · peers · files    │ ◄───────────────┐
  │  commands +     │  heartbeat checker       │   commands +    │
  │  heartbeats     └──────────────────────────┘   heartbeats    │
  │                                                              │
┌─┴──────────┐             TCP (direct transfer)          ┌──────┴─────┐
│  Client A  │ ◄────────────────────────────────────────► │  Client B  │
│  (peer)    │      GET <file>  →  OK <size> + bytes      │  (peer)    │
└────────────┘                                            └────────────┘
```

1. A client authenticates with the server and registers the TCP port on which it will serve files.
2. While logged in, the client sends a heartbeat every 2 seconds. Peers that stop sending heartbeats are treated as offline and their files stop being listed.
3. To download, the client asks the server who owns a file. The server replies with the owner's TCP port, and the client connects to that peer directly.

### Server internals

- **Hash table** (separate chaining, FNV-1a hashing) storing credentials, active peers, published files and heartbeat timestamps
- **Trie-based index** over published file names
- A `select()`-driven UDP receive loop for commands, plus dedicated threads for heartbeat handling and heartbeat expiry
- Timestamped logging of every request and response

### Client internals

- Interactive command-line shell
- Heartbeat thread (UDP)
- Embedded TCP file server thread, bound to a randomly chosen free port in the dynamic range (49152 to 65535); each incoming download request is served on its own thread
- Dynamic buffers and string-splitting helpers for parsing server replies

## Protocol

Control messages are plain-text, newline-terminated datagrams. Lists in replies are wrapped in `[...]`; file names inside a list are separated by the ASCII unit separator (`0x1F`).

| Command | Direction | Purpose | Reply |
|---------|-----------|---------|-------|
| `ATH <user> <pass> <tcp_port>` | client → server | Log in and register TCP port | `OK` / `ERR` |
| `HBT <user>` | client → server | Heartbeat (every 2 s) | none |
| `LAP <user>` | client → server | List active peers | `OK [peer,peer,...]` |
| `LPF <user>` | client → server | List published files | `OK [file...]` |
| `PUB <user> <file>` | client → server | Publish a file | `OK` |
| `UNP <user> <file>` | client → server | Unpublish a file | `OK` |
| `SCH <user> <text>` | client → server | Search published files | `OK [file...]` |
| `GET <user> <file>` | client → server | Look up a file's owner | `OK <port>` / `FNF` |
| any command from an expired session | server → client | Session no longer valid | `DSC` |

Peer-to-peer transfer over TCP:

```
requester → peer :  GET <file>\n
peer → requester :  OK <size>\n  followed by <size> raw bytes
                    ERR FNF\n   (file not found)
                    ERR BRQ\n   (bad request)
```

## Build

Requirements: a C compiler (gcc or clang) and pthreads (Linux/macOS), or MSVC / MinGW with Winsock (Windows).

```bash
# Linux / macOS
gcc -o server server.c -lpthread
gcc -o client client.c -lpthread

# Windows (MinGW)
gcc -o server.exe server.c -lws2_32
gcc -o client.exe client.c -lws2_32
```

## Run

**1. Prepare the server directory** containing a credentials file with one `username password` pair per line (up to 16 characters each):

```
server/
└── credentials.txt
```

```
alice secret1
bob   secret2
```

**2. Start the server** on a UDP port:

```bash
./server 4000
```

**3. Start clients** in separate terminals, giving the server port. Each user needs a directory named after their username, which is used as their shared and download folder:

```bash
mkdir alice
./client 4000
```

**4. Log in and use the shell:**

```
Enter username: alice
Enter password: ******
Welcome!
> pub report.txt     # publish ./alice/report.txt
> lap                # list active peers
> lpf                # list published files
> sch report         # search published files
> get report.txt     # download from the owning peer into ./alice/
> unp report.txt     # stop sharing
> xit                # quit
```

## Project Structure

```
.
├── server.c     # Index server: auth, presence, file index, heartbeat threads
├── client.c     # Peer: CLI, heartbeat sender, TCP file server, downloader
└── server/
    └── credentials.txt
```

## Skills Demonstrated

- Socket programming over both UDP and TCP, including mixed-protocol system design
- Application-layer protocol design and parsing
- Multi-threaded programming with POSIX threads and Windows threads
- Data structures in C: hash table with chaining and a trie
- Manual memory management
- Cross-platform portability

## Limitations

- Credentials are stored and compared in plain text, and traffic is unencrypted. This is a learning project, not a production system.
- The server and clients are configured for localhost (`127.0.0.1`) by default.
- UDP control messages have no retransmission, so lost datagrams are not recovered.
