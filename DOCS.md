# MiniPubSub Codebase Guide

This document explains the MiniPubSub implementation as it exists in this
repository. It is intended for maintainers, language-binding authors, and
contributors who need to follow a request from the public C API down to the
wire and back.

For design requirements and protocol guarantees, see `TRD.md`. This document
focuses on source layout, execution paths, ownership, concurrency, extension
points, and operational behavior.

## 1. What the project contains

MiniPubSub is a Linux TCP publish/subscribe system with four deliverables:

- A shared library, `libminipubsub.so`, exposing a C ABI.
- A static library, `libminipubsub.a`, exposing the same C ABI.
- An embeddable broker plus the `minipubsub-server` executable.
- An embeddable client plus the `minipubsub-cli` executable.

The implementation is C++23. C++ types, exceptions, standard-library
containers, sockets, and threads are private. Applications and foreign
language bindings interact with opaque handles and fixed-width C types.

The core product model is deliberately small:

1. A client subscribes a connection to an exact binary channel.
2. A publisher sends a binary message to a channel.
3. The broker takes the current subscriber set and queues one `MESSAGE` event
   for each eligible connection.
4. Requests receive correlated `OK`, `PONG`, or `ERROR` responses.
5. Messages are transient; there is no storage or replay.

## 2. Architecture at a glance

```text
Application or FFI runtime
          |
          | public mps_* functions
          v
+-------------------------+
| src/c_api/api.cpp       |  validation, opaque handles, diagnostics,
|                         |  exception containment, C/C++ conversion
+------------+------------+
             |
       +-----+-----------------------+
       |                             |
       v                             v
+--------------------+      +-----------------------+
| ClientEngine       |      | ServerRuntime         |
| request tracking   |      | accept + connections  |
| event delivery     |      | broker dispatch       |
+---------+----------+      +-----------+-----------+
          |                             |
          +-------------+---------------+
                        v
             +---------------------+
             | protocol codec      |
             | frame + payload I/O |
             +----------+----------+
                        |
                        v
                  POSIX TCP sockets
```

On the server side, pub/sub rules are further separated from socket handling:

```text
connection reader threads
          |
          | decoded Frame commands
          v
bounded broker-command queue
          |
          | one dispatcher thread
          v
       Broker --------------------> DeliverySink
          |                              |
          | channel registry             | immutable SharedFrame
          v                              v
  subscription state             connection output queues
                                         |
                                         v
                                  writer threads / TCP
```

The single broker dispatcher is the ordering authority. Connection threads do
not modify subscriptions directly.

## 3. Repository layout

```text
MiniPubSub/
├── CMakeLists.txt                  Build, test, install, sanitizer options
├── README.md                       Short build and CLI introduction
├── TRD.md                          Technical requirements and wire contract
├── DOCS.md                         This implementation guide
├── cmake/
│   ├── MiniPubSubConfig.cmake.in   Installed CMake package configuration
│   ├── minipubsub.pc.in            pkg-config template
│   └── symbol_exports.map          Shared-library C symbol allowlist
├── include/minipubsub/
│   ├── minipubsub.h                Umbrella public header
│   ├── types.h                     Stable types, constants, callbacks
│   ├── client.h                    Public client and event API
│   ├── server.h                    Public server API
│   └── version.h                   Source, ABI, and protocol versions
├── src/
│   ├── c_api/api.cpp               Entire C ABI adapter and opaque handles
│   ├── client/
│   │   ├── client_engine.hpp       Private client model and event type
│   │   └── client_engine.cpp       Client state, socket workers, correlation
│   ├── core/
│   │   ├── broker.hpp              Broker and DeliverySink contracts
│   │   └── broker.cpp              Subscription and fan-out behavior
│   ├── protocol/
│   │   ├── protocol_types.hpp      Opcodes, errors, frames, payload models
│   │   ├── codec.hpp               Encoder/decoder declarations
│   │   └── codec.cpp               Stream framing and payload validation
│   ├── server/
│   │   ├── server_runtime.hpp      Private server configuration and façade
│   │   └── server_runtime.cpp      Threaded accept/broker/connection runtime
│   └── cli/
│       ├── server_main.cpp         Standalone server
│       ├── client_main.cpp         One-shot and interactive client
│       ├── client_command.cpp      REPL parsing and byte formatting
│       └── client_command.hpp      Internal CLI command model
├── tests/
│   ├── unit/                       Protocol and broker tests
│   ├── integration/                Public API and raw TCP tests
│   └── abi/                        C, C++, CMake-package, and ctypes probes
└── fuzz/
    ├── frame_decoder_fuzz.cpp      Arbitrary stream input fuzz target
    └── payload_decoder_fuzz.cpp    Nested-length payload fuzz target
```

Only `include/minipubsub/` is installed for consumers. Headers below `src/`
are private and may change without an ABI version change.

## 4. Building the project

### 4.1 Normal build

The checked-in presets are the recommended development interface:

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Equivalent manual configuration remains supported:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

`CMakePresets.json` supplies `debug`, `release`, `static`, `asan`, `ubsan`, and
`tsan` configure/build/test presets. Preset output is isolated under
`build/<preset-name>/`.

The build produces:

```text
build/debug/libminipubsub.so
build/debug/libminipubsub.a
build/debug/minipubsub-server
build/debug/minipubsub-cli
```

Tests use loopback TCP sockets. An execution sandbox that denies `bind(2)` or
`connect(2)` must grant loopback socket access for integration tests.

### 4.2 CMake options

| Option                           | Default | Effect                                                      |
|----------------------------------|--------:|-------------------------------------------------------------|
| `MPS_BUILD_TESTS`                |    `ON` | Builds and registers the test programs                      |
| `MPS_BUILD_SHARED`               |    `ON` | Builds `libminipubsub.so` in addition to the static library |
| `MPS_WARNINGS_AS_ERRORS`         |    `ON` | Promotes project compiler warnings to errors                |
| `MPS_BUILD_FUZZERS`              |   `OFF` | Builds libFuzzer entry points; requires Clang               |
| `MPS_ENABLE_ADDRESS_SANITIZER`   |   `OFF` | Enables AddressSanitizer                                    |
| `MPS_ENABLE_UNDEFINED_SANITIZER` |   `OFF` | Enables UndefinedBehaviorSanitizer                          |
| `MPS_ENABLE_THREAD_SANITIZER`    |   `OFF` | Enables ThreadSanitizer; must be used alone                 |

Example sanitizer builds:

```sh
cmake --preset asan
cmake --build --preset asan
ctest --preset asan

cmake --preset tsan
cmake --build --preset tsan
ctest --preset tsan
```

Python loading an ASan-instrumented shared library may require preloading the
ASan runtime. This is a property of the Python process, not of MiniPubSub's
`ctypes` declarations.

### 4.3 Installation and downstream CMake use

```sh
cmake --install build/debug --prefix /opt/minipubsub
```

Installed CMake targets are:

```cmake
find_package(MiniPubSub 1 REQUIRED)
target_link_libraries(my_program PRIVATE MiniPubSub::shared)
# or MiniPubSub::static
```

The installation also provides `minipubsub.pc`:

```sh
cc consumer.c $(pkg-config --cflags --libs minipubsub)
```

The shared library uses SONAME `libminipubsub.so.1`. Hidden visibility is the
default, and `cmake/symbol_exports.map` exports only names matching `mps_*`.

## 5. Public C ABI

### 5.1 Why the API looks this way

The public ABI avoids C++ ABI and common FFI hazards:

- Clients, servers, configurations, and events are opaque incomplete structs.
- Statuses and constants use explicitly sized integer typedefs and macros.
- Public structures are passed through pointers, not by value.
- Byte sequences use `{pointer, uint32_t length}` rather than NUL termination.
- There are no public C enums, bitfields, variadic functions, STL types, or
  exceptions.
- `MPS_API` controls symbol visibility and `MPS_CALL` reserves a calling
  convention hook.
- Every operation that may execute C++ code is protected by an exception
  boundary.

`include/minipubsub/minipubsub.h` is the normal include. It collects the client,
server, common-type, and version headers.

### 5.2 Opaque handles

The public declarations look like this:

```c
typedef struct mps_client mps_client_t;
typedef struct mps_client_config mps_client_config_t;
typedef struct mps_server mps_server_t;
typedef struct mps_server_config mps_server_config_t;
typedef struct mps_event mps_event_t;
```

Their actual definitions live in `src/c_api/api.cpp`:

- `mps_client_config` contains the private C++ client configuration, the raw C
  callback, user data, and a consumed flag.
- `mps_server_config` contains the private server configuration and consumed
  flag.
- `mps_client` owns one `ClientEngine`.
- `mps_server` owns one `ServerRuntime` and records whether start succeeded.
- `mps_event` is defined privately beside the client engine and owns or borrows
  one internal `Event` snapshot.

Because the application never allocates these structures directly, their
private layouts can evolve without changing ABI v1.

### 5.3 Configuration lifecycle

Client and server configurations follow the same pattern:

```text
config_create -> zero or more setters -> handle_create -> config_destroy
```

`handle_create` snapshots all configuration. On success it marks the
configuration as consumed. Setters then return `MPS_STATUS_INVALID_STATE`, but
destroy remains valid. This prevents a caller from assuming that later config
mutations affect a running handle.

Configuration objects are not documented as concurrently mutable. Configure
them on one thread before creating the handle.

Client defaults:

| Setting                     |     Default |
|-----------------------------|------------:|
| Host                        | `127.0.0.1` |
| Port                        |      `7379` |
| Connect timeout             |    5,000 ms |
| Shutdown timeout            |    5,000 ms |
| Command count limit         |       1,024 |
| Internal command byte limit |       4 MiB |
| Event count limit           |       1,024 |
| Internal event byte limit   |       4 MiB |
| Event mode                  |     Polling |

Server defaults:

| Setting                       |         Default |
|-------------------------------|----------------:|
| Bind address                  |     `127.0.0.1` |
| Port                          |          `7379` |
| Maximum connections           |             128 |
| Maximum channel               |     1,024 bytes |
| Maximum message               | 1,048,576 bytes |
| Output bytes per connection   |           4 MiB |
| Broker command count          |           4,096 |
| Internal broker command bytes |          64 MiB |
| Shutdown timeout              |        5,000 ms |
| Logging                       |        Disabled |

Port zero is accepted only for the server and requests an OS-selected port.

### 5.4 Byte views and copying

```c
typedef struct mps_bytes_view {
    const uint8_t *data;
    uint32_t length;
} mps_bytes_view_t;
```

The pointer may be null only when `length` is zero. Channels cannot be empty,
so an empty channel is rejected even though it is a structurally valid byte
view. Empty messages are valid.

Command input views are borrowed for the call. A successful command copies and
encodes all input before returning, so a binding may release or move its input
buffer immediately afterward.

Event views borrow event storage:

- In polling mode they remain valid until `mps_event_destroy(event)`.
- In callback mode they remain valid only until the callback returns.

Channels and messages are binary. Do not use `strlen`, `%s`, or an automatic
UTF-8 conversion unless the application has imposed its own text convention.

### 5.5 Local status versus remote error

`mps_status_t` reports what happened inside the local library call. Important
examples are:

- `MPS_STATUS_INVALID_ARGUMENT`: bad pointer, empty host, empty channel, or
  out-of-range configuration.
- `MPS_STATUS_INVALID_STATE`: operation does not fit the handle lifecycle.
- `MPS_STATUS_TIMEOUT`: polling or connecting reached its deadline.
- `MPS_STATUS_WOULD_BLOCK`: the local outbound queue or in-flight table is full.
- `MPS_STATUS_IO_ERROR`: local name-resolution or socket failure.
- `MPS_STATUS_CLOSED`: no further events are available from a stopped client.
- `MPS_STATUS_WRONG_EVENT_MODE`: polling was attempted on a callback client.

A server rejection is different. Submission may return `MPS_STATUS_OK`, then a
later `MPS_EVENT_REMOTE_ERROR` carries the server's numeric protocol error and
human-readable message.

Most status-returning functions accept an optional `mps_diagnostic_t*`. The
adapter clears it on entry and, on failure, writes:

- `struct_size`,
- an OS or resolver error in `system_code` when applicable,
- a NUL-terminated explanatory message.

Branch on the numeric status or protocol error, not diagnostic text.

### 5.6 Exception containment

`src/c_api/api.cpp` uses the `boundary` helper around exported work. It maps:

| C++ failure            | C result                                  |
|------------------------|-------------------------------------------|
| `std::bad_alloc`       | `MPS_STATUS_OUT_OF_MEMORY`                |
| Other `std::exception` | `MPS_STATUS_INTERNAL_ERROR` with `what()` |
| Unknown exception      | `MPS_STATUS_INTERNAL_ERROR`               |

Thread entry points in the client and server also catch all exceptions. No C++
exception is intentionally allowed to cross an exported C function or escape a
library-owned thread.

## 6. Using the client API

### 6.1 Lifecycle

The internal state machine is:

```text
created -> connecting -> running -> stopping -> stopped
               |                         ^
               +------ failure ----------+
```

`mps_client_connect` resolves the configured host, performs a nonblocking
`connect`, waits with `poll(2)` up to the configured deadline, restores blocking
mode, and starts workers. It is not an asynchronous connect API.

Publish, subscribe, unsubscribe, and ping are asynchronous after connection:

1. Validate and encode the command.
2. Allocate a nonzero request ID.
3. Record the expected response kind.
4. Put the immutable encoded frame on the writer queue.
5. Return the request ID to the caller.

An `OK` return from a command means local acceptance, not broker acceptance.
The result arrives later as an event.

`mps_client_request_stop` is nonblocking and idempotent. `mps_client_wait`
drains accepted writes up to the shutdown deadline, joins all workers, and
closes the socket. `mps_client_destroy` performs stop and wait automatically.

Never call `mps_client_wait` or `mps_client_destroy` from that client's callback
thread. `mps_client_request_stop` is callback-safe.

### 6.2 Polling-mode example

This abbreviated example subscribes and waits for the correlated result:

```c
#include <minipubsub/minipubsub.h>
#include <stdint.h>

int subscribe_example(uint16_t port) {
    mps_client_config_t *config = NULL;
    mps_client_t *client = NULL;
    mps_event_t *event = NULL;
    mps_diagnostic_t diagnostic = {0};
    mps_request_id_t request_id = 0;
    static const uint8_t channel_bytes[] = {'n', 'e', 'w', 's'};
    const mps_bytes_view_t channel = {
        channel_bytes, (uint32_t)sizeof(channel_bytes)
    };

    if (mps_client_config_create(&config, &diagnostic) != MPS_STATUS_OK ||
        mps_client_config_set_port(config, port, &diagnostic) != MPS_STATUS_OK ||
        mps_client_create(config, &client, &diagnostic) != MPS_STATUS_OK) {
        mps_client_config_destroy(config);
        return 1;
    }
    mps_client_config_destroy(config);

    if (mps_client_connect(client, &diagnostic) != MPS_STATUS_OK ||
        mps_client_subscribe(client, &channel, &request_id, &diagnostic) !=
            MPS_STATUS_OK) {
        mps_client_destroy(client);
        return 1;
    }

    for (;;) {
        mps_status_t status =
            mps_client_poll_event(client, 5000, &event, &diagnostic);
        if (status != MPS_STATUS_OK) {
            mps_client_destroy(client);
            return 1;
        }
        if (mps_event_kind(event) == MPS_EVENT_OK &&
            mps_event_request_id(event) == request_id) {
            mps_event_destroy(event);
            break;
        }
        mps_event_destroy(event);
    }

    mps_client_destroy(client);
    return 0;
}
```

Exactly one thread may call `mps_client_poll_event` for a given client at a
time. The engine enforces this with a nonblocking `poll_mutex`; concurrent polls
return `MPS_STATUS_INVALID_STATE`.

### 6.3 Callback mode

Calling `mps_client_config_set_event_callback` selects callback mode. After the
client connects, a dedicated dispatcher thread serially invokes the callback:

```c
struct callback_context {
    mps_client_t *client;
};

static void on_event(const mps_event_t *event, void *user_data) {
    struct callback_context *context = user_data;

    if (mps_event_kind(event) == MPS_EVENT_MESSAGE) {
        mps_bytes_view_t channel = {0};
        mps_bytes_view_t message = {0};
        (void)mps_event_channel(event, &channel);
        (void)mps_event_message(event, &message);
        /* Consume or copy channel/message before returning. */
    }

    if (mps_event_kind(event) == MPS_EVENT_DISCONNECTED) {
        (void)mps_client_request_stop(context->client, NULL);
    }
}
```

Create the context before configuring the callback, pass its address as
`user_data`, and assign `context.client` immediately after
`mps_client_create` returns. The client cannot invoke the callback until a
successful `mps_client_connect` starts its dispatcher.

Callbacks are never invoked while a client queue mutex is held. They may submit
new commands and request stop. A callback must not retain or destroy its event.
`mps_event_destroy` intentionally deletes only owned polling events.

The callback must remain fast. While it runs, the reader may continue adding
events. If the bounded event queue fills, the engine closes the connection and
tries to preserve a final `MPS_EVENT_DISCONNECTED` with cause
`MPS_DISCONNECT_QUEUE_LIMIT`.

### 6.4 Event accessors

| Event kind               | Request ID    | Channel/message | Protocol error | Disconnect cause |
|--------------------------|---------------|-----------------|----------------|------------------|
| `MPS_EVENT_OK`           | Correlated ID | Not applicable  | 0              | 0                |
| `MPS_EVENT_PONG`         | Correlated ID | Not applicable  | 0              | 0                |
| `MPS_EVENT_MESSAGE`      | 0             | Available       | 0              | 0                |
| `MPS_EVENT_REMOTE_ERROR` | Correlated ID | Not applicable  | Available      | 0                |
| `MPS_EVENT_DISCONNECTED` | 0             | Not applicable  | 0              | Available        |

`mps_event_channel` and `mps_event_message` fill the output view and return
`MPS_STATUS_INVALID_STATE` when called on a non-message event. Scalar accessors
return zero for a null or non-applicable event field.

## 7. Using the server API

Typical embedded lifecycle:

```c
mps_server_config_t *config = NULL;
mps_server_t *server = NULL;
uint16_t actual_port = 0;

mps_server_config_create(&config, NULL);
mps_server_config_set_port(config, 0, NULL);
mps_server_create(config, &server, NULL);
mps_server_config_destroy(config);

mps_server_start(server, NULL);
mps_server_bound_port(server, &actual_port, NULL);

/* Application work. */

mps_server_request_stop(server, NULL);
mps_server_wait(server, NULL);
mps_server_destroy(server);
```

`mps_server_start` returns only after name resolution, socket creation, bind,
listen, and bound-port discovery have succeeded. It then starts the acceptor and
broker threads.

The internal state machine is:

```text
created -> starting -> running -> stopping -> stopped
               |                         ^
               +------ failure ----------+
```

The default bind address is loopback. Binding to `0.0.0.0` or `::` requires an
explicit configuration choice.

### 7.1 Logging callback

The server is silent unless a log callback is configured. A record contains
severity, component, event name, connection ID, and a human-readable message.

The callback can be invoked from the acceptor, broker, reader, or writer thread,
so application logging code must be thread-safe. Record strings and the record
itself are borrowed for the call. Arbitrary channel and message bytes are not
logged.

## 8. Wire protocol implementation

### 8.1 Common frame

Every frame begins with 16 bytes:

```text
offset  size  field
0       4     magic       4d 50 53 00 ("MPS\0")
4       1     version     01
5       1     opcode
6       2     flags       big-endian, zero in v1
8       4     request_id  big-endian
12      4     payload_len big-endian
16      ...   payload
```

All integers are encoded explicitly by `read_u16`, `read_u32`, `write_u16`, and
`write_u32` in `src/protocol/codec.cpp`; the implementation does not cast an
unaligned network buffer to a C++ struct.

Opcodes:

|  Value | Name          | Direction        |
|-------:|---------------|------------------|
| `0x01` | `PUBLISH`     | Client to server |
| `0x02` | `SUBSCRIBE`   | Client to server |
| `0x03` | `UNSUBSCRIBE` | Client to server |
| `0x04` | `PING`        | Client to server |
| `0x80` | `OK`          | Server to client |
| `0x81` | `MESSAGE`     | Server to client |
| `0x82` | `ERROR`       | Server to client |
| `0x83` | `PONG`        | Server to client |

### 8.2 Payloads

```text
channel:
    channel_len:u32
    channel:channel_len bytes

channel + message:
    channel_len:u32
    channel:channel_len bytes
    message_len:u32
    message:message_len bytes

error:
    error_code:u16
    message_len:u32
    message:message_len bytes
```

The payload decoders verify minimum size, configured field limits, exact nested
lengths, and absence of trailing bytes before constructing typed values.

### 8.3 Streaming decoder

Each TCP connection owns a `FrameDecoder`. `consume` works as follows:

1. Append newly read bytes to `pending_`.
2. Stop if fewer than 16 header bytes are present.
3. Validate magic and version before trusting `payload_len`.
4. Reject a declared payload larger than that connection's configured maximum.
5. Stop without error if the complete payload has not arrived yet.
6. Copy a complete frame into a typed `Frame` and continue, permitting multiple
   frames per read.
7. Erase consumed bytes while retaining an incomplete suffix.

Fatal magic, version, or common-length errors latch the decoder into a fatal
state and clear pending bytes. Nested payload errors are handled later by the
broker or client response validator because common framing is still usable.

### 8.4 Immutable encoded frames

`SharedFrame` is:

```cpp
std::shared_ptr<const std::vector<std::uint8_t>>
```

The broker encodes one `MESSAGE` frame per publication and shares that immutable
storage among subscriber output queues. Each queue still charges the full frame
size against its own byte limit, even though physical payload storage is shared.

## 9. Broker internals

`src/core/broker.cpp` contains no socket or public-handle code. Its outward
dependency is the small `DeliverySink` interface:

```cpp
class DeliverySink {
public:
    virtual bool enqueue(ConnectionId, protocol::SharedFrame) = 0;
    virtual void close(ConnectionId, CloseReason) = 0;
};
```

The broker owns two indexes:

```text
channels_: channel -> set<ConnectionId>
reverse_:  ConnectionId -> set<channel>
```

The forward index makes fan-out direct. The reverse index makes connection
cleanup proportional to that connection's subscriptions rather than to every
channel in the broker.

All broker methods execute on one dispatcher thread, so these maps require no
internal mutex. This is a central invariant: calling `Broker::execute` directly
from connection reader threads would introduce races and destroy global
publication ordering.

### 9.1 Command behavior

- `SUBSCRIBE` inserts into both indexes and then queues `OK`. Set insertion makes
  duplicate subscribe idempotent.
- `UNSUBSCRIBE` erases from both indexes, removes empty channel entries, and
  returns `OK` even when no subscription existed.
- `PING` requires an empty payload and returns `PONG`.
- `PUBLISH` decodes once, snapshots the subscriber set, encodes one shared
  message frame, attempts every subscriber queue, closes slow subscribers, then
  queues publisher `OK`.
- `connection_closed` walks the reverse index and removes every subscription.
- An unknown client opcode returns a recoverable `UNKNOWN_OPCODE` error.
- Reserved flags or response-direction opcodes produce an error and request a
  protocol close after queued output is drained.

The subscriber set is copied before fan-out. This allows close requests to be
generated during iteration without invalidating the set currently being walked.

## 10. Server runtime

`ServerRuntime` is a small PIMPL façade. Almost all behavior resides in
`ServerRuntime::Impl` in `src/server/server_runtime.cpp`.

### 10.1 Threads

For a running server:

| Thread            |            Count | Responsibility                                                 |
|-------------------|-----------------:|----------------------------------------------------------------|
| Acceptor          |                1 | Block in `accept`, allocate connection IDs, create connections |
| Broker dispatcher |                1 | Serialize open/frame/close commands and own broker state       |
| Connection reader | 1 per connection | `recv`, frame decoding, bounded command submission             |
| Connection writer | 1 per connection | Wait for queued frames and perform complete writes             |

No worker thread is detached.

Connection IDs come from a monotonically increasing 64-bit atomic counter. File
descriptors are never used as broker identities because an OS may quickly reuse
a closed descriptor.

### 10.2 Accept path

`start` resolves the bind address with `getaddrinfo`, tries candidate sockets,
enables `SO_REUSEADDR`, binds, listens, and calls `getsockname` to discover a
port selected from configuration value zero.

The acceptor checks the active-connection limit. Accepted sockets beyond the
limit are closed immediately. For an admitted socket it:

1. Allocates a connection ID.
2. Creates a `Connection` containing a decoder and output queue.
3. Retains it in the runtime connection map.
4. Submits an `opened` command to the broker queue.
5. Starts its reader and writer.

### 10.3 Broker command queue

Reader threads submit `opened`, decoded `frame`, and `closed` commands through a
mutex/condition-variable queue. It is bounded by configured command count and a
64 MiB internal retained-byte budget. A single valid command larger than that
budget is allowed only when the queue is empty; this prevents an otherwise
valid configured large message from waiting forever.

When the queue is full, a reader waits. That stops reads from its TCP connection
and lets kernel/TCP flow control apply backpressure to the sender. Stop wakes
blocked producers.

### 10.4 Per-connection output queue

Each `Connection` has:

- an output deque of `SharedFrame`,
- an exact queued-byte counter,
- a mutex and condition variable,
- atomic stopping, read-stop, draining, and close-report flags,
- one decoder,
- reader and writer thread objects.

`enqueue` never performs a socket write. If admitting the whole frame would
cross the configured output-byte limit, it returns false. The broker interprets
that as a slow subscriber and closes only that connection.

The writer handles short writes and `EINTR`. `MSG_NOSIGNAL` prevents peer
disconnects from delivering `SIGPIPE` to the embedding process. `SO_SNDTIMEO`
helps bound a blocking send during shutdown.

### 10.5 Descriptor lifetime

Reader and writer threads can finish concurrently. Closing a descriptor while
the other worker is inside `recv` or `send` risks both data races and descriptor
reuse bugs. The connection therefore tracks `live_threads_`; the last exiting
worker atomically takes and closes the descriptor. The destructor has a fallback
close after joining.

`closed_reported_` similarly guarantees exactly one broker `closed` command and
one active-connection decrement even if read failure, write failure, queue
overflow, and shutdown race.

### 10.6 Graceful shutdown

Server stop proceeds in phases:

1. Change state to stopping and `shutdown` the listening socket to wake accept.
2. Signal all connection readers with `SHUT_RD`.
3. Join the acceptor and close the listening descriptor.
4. Join readers; their final `closed` commands follow previously accepted frames.
5. Let the broker dispatcher drain its queue, then join it.
6. Tell writers to drain their admitted output queues.
7. Start a shutdown-deadline watchdog; if writers do not finish, force socket
   shutdown.
8. Join writers, release connection objects, and enter stopped state.

This ordering lets already-decoded broker commands run before subscription
cleanup and lets already-admitted output drain when peers remain responsive.

## 11. Client engine

`ClientEngine` also uses PIMPL. Its private state combines transport, request
tracking, and event dispatch.

### 11.1 Threads

| Thread     | Polling mode | Callback mode | Responsibility                     |
|------------|-------------:|--------------:|------------------------------------|
| Reader     |            1 |             1 | Decode responses and create events |
| Writer     |            1 |             1 | Drain the command queue to TCP     |
| Dispatcher |            0 |             1 | Invoke callbacks serially          |

The application thread calls command methods and, in polling mode, consumes
events. Command methods are serialized by `command_mutex` and are safe to call
concurrently.

### 11.2 Request IDs and in-flight tracking

The client maintains:

```cpp
std::unordered_map<uint32_t, Expected> in_flight;
uint32_t next_request_id{1};
```

`Expected` is either `ok` or `pong`. IDs increase monotonically, wrap from
`UINT32_MAX` to 1, skip zero, and skip any ID still present in the table.

Queue-full checks happen before ID allocation and encoding. The configured
command limit bounds both queued frames and in-flight requests. The internal
4 MiB byte limit additionally bounds encoded frames waiting for the writer.

### 11.3 Response validation

The reader gives each decoded server frame to `handle_frame`:

- `OK` must have an empty payload and match an in-flight request expecting OK.
- `PONG` must have an empty payload and match one expecting PONG.
- `ERROR` must match any in-flight request and have a valid error payload.
- `MESSAGE` must use request ID zero and contain a valid channel/message payload.
- Client-to-server opcodes, nonzero flags, unknown IDs, duplicate IDs, wrong
  response kinds, and malformed payloads are protocol violations.

Valid responses remove their in-flight entry before entering the event queue.
Messages bypass correlation.

### 11.4 Event queue

The event queue is bounded by configured event count and an internal 4 MiB byte
budget. Its accounting includes the `Event` object, channel, message, and error
text.

Polling moves one internal event into a heap-allocated opaque `mps_event` owned
by the caller. Callback dispatch moves one event to dispatcher-local storage and
passes it through a temporary borrowed C wrapper.

No callback is run while `event_mutex` is held.

### 11.5 Client shutdown

`request_stop` takes the command mutex, moves to stopping, rejects future
commands, and wakes the writer. The writer drains already accepted frames, then
half-closes its write side. `wait` runs a deadline watchdog that forces full
socket shutdown if the writer stalls. It then joins reader and dispatcher,
closes the descriptor, and enters stopped state.

The reader emits a single disconnected event with one of these causes:

- requested shutdown,
- remote close,
- local I/O failure,
- protocol violation,
- event queue limit.

## 12. End-to-end flows

### 12.1 Subscribe

```text
application
  -> mps_client_subscribe
  -> C adapter validates byte view
  -> ClientEngine validates channel and allocates request ID
  -> encode_channel + encode_frame(SUBSCRIBE)
  -> client command queue
  -> client writer -> TCP
  -> server reader -> FrameDecoder
  -> server broker-command queue
  -> Broker updates both subscription indexes
  -> DeliverySink queues OK with the same request ID
  -> server writer -> TCP
  -> client reader validates request correlation
  -> event queue
  -> polling caller or callback receives MPS_EVENT_OK
```

The subscription is installed before the server queues `OK`, so a publication
processed after this subscribe may produce a message even if the subscriber has
not consumed its `OK` yet.

### 12.2 Publish

```text
publisher command
  -> PUBLISH frame
  -> broker dispatcher
  -> validate channel/message
  -> snapshot current subscriber IDs
  -> encode one shared MESSAGE frame
  -> enqueue it independently to each subscriber
  -> close subscribers that exceed output limit
  -> enqueue OK to publisher
```

A connection may be both publisher and subscriber. In that case its `MESSAGE`
is queued before its publication `OK`, matching broker processing order.

`OK` confirms admission to eligible server output queues. It does not confirm
that subscribers have read, processed, or persisted anything.

### 12.3 Malformed or hostile input

```text
bad magic/version/common payload length
  -> decoder marks fatal
  -> immediate connection shutdown, no declared-payload allocation

valid common frame + malformed nested payload
  -> broker sends ERROR
  -> connection remains usable

unknown opcode in a valid frame
  -> broker sends UNKNOWN_OPCODE
  -> connection remains usable

nonzero flags or response opcode sent by client
  -> broker queues INVALID_REQUEST
  -> output drains
  -> connection closes
```

## 13. Ownership and concurrency reference

| Object/data                   | Owner           | Threading and lifetime                                |
|-------------------------------|-----------------|-------------------------------------------------------|
| Client/server config          | Caller          | Single-threaded setup; destroy after handle creation  |
| Client/server handle          | Caller          | Operations follow documented concurrent-safe subset   |
| Command byte view             | Caller          | Borrowed during call; copied before successful return |
| Poll event                    | Caller          | Owned until `mps_event_destroy`                       |
| Callback event                | Library         | Borrowed only during callback                         |
| Event byte view               | Event           | Invalid when owning event lifetime ends               |
| Protocol `Frame`              | Decoder/command | Moved through internal queues                         |
| `SharedFrame`                 | Shared pointer  | Immutable; shared across fan-out queues               |
| Broker registries             | Broker thread   | No concurrent access by design                        |
| Server connection map         | Server runtime  | Protected by `connections_mutex`                      |
| Broker command queue          | Server runtime  | Protected by `command_mutex`                          |
| Connection output queue       | Connection      | Protected by `output_mutex_`                          |
| Client commands/in-flight IDs | Client engine   | Protected by `command_mutex`                          |
| Client events                 | Client engine   | Protected by `event_mutex`                            |

Condition-variable predicates always include a stop/drain condition so shutdown
can wake otherwise idle workers.

## 14. CLI programs

### 14.1 Server

```sh
./build/minipubsub-server
./build/minipubsub-server --bind 127.0.0.1 --port 7379
./build/minipubsub-server --port 0
```

The server CLI installs a structured log callback that writes to stderr, prints
the bound endpoint to stdout, and responds to `SIGINT` and `SIGTERM` by requesting
stop and waiting for shutdown.

### 14.2 Client

With no positional command, the client opens an interactive session:

```sh
./build/debug/minipubsub-cli --host 127.0.0.1 --port 7379
> SUBSCRIBE news
OK
> PUBLISH news "hello world"
!msg "news": hello world
OK
> PUBLISH news
ERROR: Invalid Argument
> EXIT
bye. :)
```

Commands are case-insensitive. `PING`, `PUBLISH`, `SUBSCRIBE`, `UNSUBSCRIBE`,
`HELP`, `EXIT`, and `QUIT` are supported. Each broker command waits up to 30
seconds for its correlated response. Publications can arrive between commands
or while a command is waiting; the callback prints them immediately and redraws
the prompt when it was visible.

Arguments use shell-like grouping. Single quotes preserve their contents,
double quotes group text, and adjacent quoted and unquoted fragments form one
argument. Backslash escapes support `\\`, `\\"`, `\\'`, `\\n`, `\\r`, `\\t`,
`\\0`, and `\\xHH`. For example, `PUBLISH binary "a\\0b"` sends a three-byte
message. An explicit empty message is written as `PUBLISH channel ""`.

Received bytes are rendered safely: printable ASCII remains readable and
quotes, backslashes, controls, and non-printable bytes are escaped. The channel
is quoted in interactive message output. Prompt redraw does not reconstruct
partially typed terminal text, although that input remains available to the
terminal and is still submitted normally.

Supplying a positional command selects the existing one-shot mode:

```sh
./build/debug/minipubsub-cli ping
./build/debug/minipubsub-cli publish news "hello"
./build/debug/minipubsub-cli subscribe news
./build/debug/minipubsub-cli unsubscribe news

./build/debug/minipubsub-cli --host 127.0.0.1 --port 7379 ping
```

The CLI deliberately uses only the installed C API. This makes it a small real
consumer of the ABI rather than a privileged path into internal C++ classes.
One-shot command-line arguments are text. Interactive escapes can represent
binary channels and messages supported by the underlying library.

## 15. Test suite

| Test                                       | What it proves                                                                                                               |
|--------------------------------------------|------------------------------------------------------------------------------------------------------------------------------|
| `tests/unit/protocol_tests.cpp`            | Golden header bytes, one-byte fragmentation, coalesced frames, fatal headers, limits, trailing bytes                         |
| `tests/unit/broker_tests.cpp`              | Duplicate subscribe, idempotent unsubscribe, fan-out, shared frame identity, disconnect cleanup, invalid request IDs         |
| `tests/unit/cli_command_tests.cpp`         | Interactive command casing, quoting, escapes, arity validation, and safe byte formatting                                   |
| `tests/integration/integration_tests.cpp`  | Port-zero server, binary pub/sub, ping, concurrent publishers, callback dispatch, callback-safe stop, active-client shutdown |
| `tests/integration/cli_tests.cpp`          | Interactive process I/O, responses, message delivery, invalid input, help, exit, and one-shot compatibility                  |
| `tests/integration/tcp_protocol_tests.cpp` | Real fragmented raw TCP, recoverable unknown opcode, flags error then close, fatal bad magic                                 |
| `tests/abi/c_consumer.c`                   | Strict C header and shared-library consumption                                                                               |
| `tests/abi/cpp_consumer.cpp`               | C headers remain valid in C++                                                                                                |
| `tests/abi/python_ctypes_poll.py`          | Dynamic loading and polling from Python                                                                                      |
| `tests/abi/python_ctypes_callback.py`      | FFI callback and opaque user-data round trip                                                                                 |
| `tests/abi/package_consumer/`              | Installed `find_package` and imported target                                                                                 |
| `fuzz/*.cpp`                               | Arbitrary common-frame and nested-payload input entry points                                                                 |

To run one test with verbose output:

```sh
ctest --test-dir build -R tcp_protocol --output-on-failure -V
```

The raw TCP test is the best starting point when changing framing or connection
close behavior. The broker unit test is the fastest feedback loop for pub/sub
semantics. The integration test is the broadest concurrency and lifecycle test.

## 16. Adding or changing functionality

### 16.1 Adding a wire command

Work from the bottom up:

1. Add an opcode and any typed payload model in `protocol_types.hpp`.
2. Add exact encoder/decoder functions in `codec.hpp` and `codec.cpp`.
3. Add golden, fragmentation, malformed-length, and limit tests.
4. Add broker semantics in `Broker::execute`.
5. Add client request expectation and response/event handling.
6. Add opaque C API functions and constants without exposing C++ types.
7. Add C, C++, and `ctypes` probes for any new ABI surface.
8. Update `TRD.md` if the wire or ABI contract changes.

Do not reuse an existing opcode or change a v1 payload in place. A wire-
incompatible change requires a new protocol version.

### 16.2 Adding a C API option

Prefer a new setter on an opaque configuration object. Avoid adding public
configuration structure fields, passing structs by value, or publishing a C++
class. New exported functions must use the `mps_` prefix and be covered by an
ABI consumer.

Within ABI major 1, do not change existing numeric constants, signatures,
ownership rules, callback thread rules, or structure layouts.

### 16.3 Replacing the threaded I/O backend

The protocol layer and broker are already independent of file descriptors. The
broker's `DeliverySink` is the main seam between pub/sub behavior and output.

The current v1 server transport mechanics—accept, connection objects, socket
queues, and reader/writer threads—are nested in `ServerRuntime::Impl` rather than
implemented as a separate `ServerTransport` class. A future epoll or io_uring
backend should first extract these responsibilities behind an ordered transport
observer and send/close interface. The broker and codec should not change during
that extraction.

The client currently combines socket transport, correlation, and events inside
`ClientEngine::Impl`. A future event-loop client should similarly extract raw
connect/read/write mechanics while retaining request tracking and event rules.

## 17. Current boundaries and known limitations

These are intentional product limits or concrete properties of the current
implementation:

- Runtime support is Linux/POSIX only.
- There is no authentication, authorization, TLS, persistence, replay,
  clustering, wildcard subscription, delivery acknowledgement, or consumer
  group support.
- Channels and messages are opaque bytes; the CLI is text-oriented only.
- The client enforces the v1 default 1 KiB channel and 1 MiB message maxima.
  Server maxima are configurable, so a server may be configured more strictly;
  a looser server configuration does not increase the current client limit.
- The server uses two threads per accepted connection and is intentionally
  tuned for a modest default connection limit.
- Finished connection objects remain in the server's connection map until
  server shutdown. Their sockets are closed and active count is decremented,
  but a long-running server with extreme connection churn retains the finished
  connection/thread objects until `mps_server_wait`. Reaping completed
  connections during runtime is a useful follow-up improvement.
- The server log callback may run concurrently from multiple internal threads.
- Configuration setters expose queue count limits; internal byte ceilings for
  client command/events and server broker commands are currently fixed defaults.
- Metrics counters are not part of the public API.
- Official language packages are not included. The Python files are ABI probes,
  not a supported Python client distribution.

## 18. Recommended reading order

For a first code review, read in this order:

1. `include/minipubsub/types.h` and `include/minipubsub/client.h` to understand
   the consumer contract.
2. `src/protocol/protocol_types.hpp` and `src/protocol/codec.cpp` to understand
   bytes on the wire.
3. `src/core/broker.cpp` to understand all pub/sub rules.
4. `src/server/server_runtime.cpp`, following `accept_loop`, `Connection`,
   `submit`, and `broker_loop`.
5. `src/client/client_engine.cpp`, following `submit`, `writer_loop`,
   `reader_loop`, `handle_frame`, and `callback_loop`.
6. `src/c_api/api.cpp` to see how ownership, errors, and callbacks cross the ABI.
7. The unit tests, then the integration and raw TCP tests, to see executable
   examples of the contracts above.

That path moves from stable public concepts to wire representation, pure
business behavior, concurrent execution, and finally edge-case verification.
