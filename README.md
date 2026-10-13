# 🌐 Concurrent HTTP/1.1 Server in C

**A web server written from raw sockets up** — no frameworks, no
libraries doing the work. It parses HTTP itself, serves and stores
files, and handles many clients at once with a thread pool.

## What it does

- **GET** a file → `200 OK` with its contents (or `404` / `403`)
- **PUT** a file → `201 Created` for a new file, `200 OK` when
  overwriting one
- Correct, deliberate error handling: `400` for malformed requests,
  `501` for unsupported methods, `505` for non-1.1 versions
- Every request is written to a thread-safe audit log:
  `METHOD,URI,STATUS,REQUEST_ID`

## Concurrency design

- A dispatcher thread accepts connections and pushes them onto a
  **bounded blocking queue** (capacity 128, mutex + condition
  variables) — a worker pool of up to 128 threads drains it
- **Striped file locks:** files are protected by 128 mutex buckets
  chosen by a djb2 hash of the URI, so requests for *different*
  files run in parallel while requests for the *same* file
  serialize — no global lock bottleneck, no torn reads/writes
- Run with `-p 1` and it degrades gracefully to a single-threaded
  server

## Parsing and safety

- Strict request validation: method, URI charset and length
  (`[A-Za-z0-9.-]`, max 63 chars — which also makes path traversal
  impossible), header keys/values, and a fully-checked
  `Content-Length`
- Bodies that arrive partly with the headers and partly later are
  reassembled correctly
- `SIGPIPE` is ignored and all sends/receives loop over partial
  I/O and `EINTR`, so a flaky client can't kill the server

## Build and run

```bash
make
./httpserver -p 4 8080     # 4 worker threads, port 8080
```

```bash
curl -X PUT --data-binary 'hello' http://localhost:8080/hello.txt
curl http://localhost:8080/hello.txt
```

## Files

- `main.c` — listen socket, argument parsing, dispatcher + worker pool
- `http.c` — request parser, GET/PUT handlers, audit log
- `queue.c` / `queue.h` — bounded blocking work queue
- `util.c` / `util.h` — `send_all` / `read_all` / strict int parsing

## Known limits

- No read timeout on client sockets — a client that connects and
  sends nothing can hold a worker thread (a deliberate scope
  choice for this version)
- Request bodies are buffered in memory, so extremely large
  `Content-Length` values are bounded by RAM, not by a configured cap
