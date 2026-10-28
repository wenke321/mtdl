# mtdl — multi-threaded HTTP(S) downloader (C++17, Linux)

A segmented, resumable downloader that opens N parallel connections to a mirror,
writes disjoint byte ranges straight into the output file, and — this is the
point of the design — **spreads those connections over every edge address the
mirror publishes**. No third-party libraries beyond OpenSSL (TLS) and pthreads.

```
build/mtdl <url> [-o out] [-c 96] [--fanout] [--adaptive] ...
```

## What limits speed (and what a client can do about it)

Measured with `ss -tin` during a download, every connection sits at
`delivery_rate ~0.5 Mbit/s  app_limited`: the sender's byte stream, not
congestion control, not the client, sets the pace. That per-connection
allowance is roughly constant (~0.05–0.07 MiB/s), so throughput ≈
`connections × allowance` **until the mirror stops handing out more**. Where
that stop happens moved between two regimes during testing on this machine
(3 vCPU, Aliyun mirror, 155 MiB kernel tarball):

**Regime 1 — the mirror capped per edge address.** 8 connections to one address
were as fast as 32 (0.5 MiB/s), and each of the ~12 edge addresses had its own
allowance:

| what was varied | connections | edges | total |
|---|---|---|---|
| one edge, 2 → 4 → 8 conns | 8 | 1 | 0.12 → 0.26 → **0.52 MiB/s** |
| one edge, 16 → 32 conns | 32 | 1 | **0.44–0.52** (saturated) |
| 12 processes, one per edge, 4 conns each | 48 | 12 | **2.99** |
| one process, fan-out (this client) | 96 | 18 | **4.72** |
| one process, `--no-fanout` (single edge) | 32 | 1 | 2.05 |

**Regime 2 — the mirror capped per connection only** (a few minutes later, same
URL, same box, interleaved runs): at equal connection counts both strategies are
equal, and the ceiling is ~5 MiB/s:

| connections | single edge | fan-out |
|---|---|---|
| 96 | 4.84 then 3.97 MiB/s | 4.45 then 4.41 MiB/s (12 edges) |

So fan-out is a **hedge, not a universal multiplier**: it is worth 2–9x while the
mirror applies a per-edge cap (and it was the whole story for the first half of
this session), and it costs nothing measurable when the cap is per connection.
It also removes the lottery of *which* address the resolver happened to return:
one slow or dead edge can no longer cap the whole download, it just gets
retired. Because the regime changes without notice, the client ramps
adaptively and keeps the per-edge limit a *soft* preference only.

### Full-file runs with the final defaults

| run | connections | edges | wall time | avg speed | integrity |
|-----|-------------|-------|-----------|-----------|-----------|
| default (`--adaptive`, ceiling 96) | 96 | 19 | 30.3 s | **5.11 MiB/s** | `xz -t` OK |
| default | 96 | 12 | 42.5 s | 3.64 MiB/s | `xz -t` OK |
| interrupt at 10 s, then resume | 96 | — | 50.3 s total | — | **sha256 identical** to the run above |

The spread between those two rows is the mirror (its allowance drifts), not the
client: identical command, identical code, ten minutes apart.

## Architecture

Strict layering; each layer is replaceable on its own.

```
util/     error.hpp log.hpp strings.hpp     errors, logging, formatting
net/      endpoint.hpp                      one edge address (ip, port, family)
          resolver.cpp                      multi-A/AAAA enumeration (parallel
                                            lookups, because the mirror rotates
                                            the subset each query returns)
          url.cpp                           URL parsing (http/https, IPv6)
          socket.cpp                        connect() with timeout, TCP_NODELAY,
                                            SO_RCVTIMEO; connect_endpoint() skips
                                            DNS so 100 workers never serialise
                                            on the resolver
          stream.hpp stream.cpp             ByteStream interface + PlainStream
          tls.cpp                           TLS: SNI, cert/hostname verify,
                                            refcounted client session cache,
                                            RELEASE_BUFFERS
          buffered.cpp                      read-ahead so header parsing never
                                            eats body bytes
net/http/ request.cpp response.cpp client.cpp HTTP/1.1: range requests, keep-alive,
                                            Content-Length + chunked decoding,
                                            pinned-endpoint connect
core/     prober.cpp                        one-byte probe -> size, etag,
                                            range support
          endpoint_pool.cpp                 owns the edge set; leases one edge per
                                            worker (least loaded, prefers edges
                                            below the soft limit), tracks bytes
                                            and retires edges that never deliver
          segment.cpp                       WorkQueue: dynamic chunk scheduler +
                                            requeue of abandoned ranges
          state.cpp                         resume bitmap (crash-safe file)
          file_sink.cpp                     preallocated file + pwrite
          progress.cpp                      live speed/ETA sampler thread
          worker.cpp                        one connection: lease an edge, retry,
                                            resume mid-chunk, re-lease on failure
          downloader.cpp                    orchestration, adaptive ramp
          config.cpp                        CLI
```

Data flow:

1. **Probe** — `GET` with `Range: bytes=0-0`; learn size from `Content-Range`,
   plus `ETag`/`Last-Modified`. If the server ignores ranges (HTTP 200), the
   downloader degrades to a single connection instead of failing.
2. **Edges** — `Resolver` asks the name server repeatedly (8 concurrent lookups
   by default) and merges the answers; for `mirrors.aliyun.com` that turns 3–4
   addresses into ~19. IP literals and `--connect-to/--endpoints` skip DNS.
3. **Plan** — resume granularity is a small *block* (64 KiB, grown if the file
   is huge so the bitmap stays small). `WorkQueue` hands out runs of blocks,
   with the run length shrinking as the file finishes.
4. **Workers** — each worker *leases* an edge from `EndpointPool`, opens one
   TCP/TLS connection to it, streams chunks with a 256 KiB buffer, `pwrite`s at
   the absolute offset, marks finished blocks and takes the next chunk on the
   same connection (keep-alive). Leases are given to the least busy edge, so
   connections fan out over the whole set without any central scheduler.
5. **Failure handling** — a dropped connection resumes at the last byte written;
   an edge that cannot deliver even the first byte twice is retired and its
   worker leases another edge; a worker that runs out of retries *requeues* the
   unread tail so other connections finish it instead of the whole download
   stalling.
6. **Resume** — completed blocks are published when their last byte is written
   (never earlier) and flushed to `<file>.mtdl-state` on exit/interrupt.
   Re-running the same command skips them.
7. **Finalise** — `fsync`, verify every block is present, then `rename()` the
   `.part` file into place (atomic) and delete the state file.

### Two details that matter for speed

* **Edge fan-out.** The single biggest lever: throughput scales with the number
  of *edges* in use, not with the number of sockets on one address.
* **Adaptive ramp with an EMA.** Start at `--min-connections` (8), then add
  batches every second while measured throughput still rises by >3 %; stop
  after two stagnant samples. That finds the knee where the mirror's aggregate
  ceiling starts to dominate, so a fast single-edge host still gets all `-c`
  connections while a 19-edge mirror stops at ~90 instead of opening 300 idle
  sockets.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Requires: g++ ≥ 9 (C++17), libssl-dev, CMake ≥ 3.16.

## Usage

```bash
# just download (fan-out + adaptive are on by default)
build/mtdl https://mirrors.aliyun.com/linux-kernel/v7.x/linux-7.3.tar.xz

# raise/lower the ceiling; the ramp still stops at the knee
build/mtdl <url> -c 128 --conns-per-edge 8

# one address only (old behaviour, useful as a benchmark baseline)
build/mtdl <url> --no-fanout -c 32

# more edges (some mirrors expose more records than 12 lookups reveal)
build/mtdl <url> --dns-lookups 40

# hand-picked edges, e.g. when DNS is broken or behind a broken load balancer
build/mtdl <url> --endpoints 183.60.240.56,183.60.240.57,[2001:db8::1]:443

# self-signed / cert-less mirrors
build/mtdl <url> --insecure

# bench fan-out vs single edge
scripts/bench.sh https://mirrors.aliyun.com/linux-kernel/v7.x/linux-7.3.tar.xz
```

Options: see `build/mtdl --help`.

Exit codes: `0` complete, `1` incomplete (state saved, re-run to resume),
`2` fatal error, `130` interrupted with Ctrl-C.

## Testing done

`scripts/selftest.sh` runs end to end against a local range-capable HTTP server
(now with keep-alive, per-connection rate limiting and path-based 404s) and
checks byte-exactness with `sha256sum` after every scenario:

* 1 connection, 32 connections, adaptive ramp, 512 KiB chunks.
* Ctrl-C mid-download → resume → byte-exact (both on a fast server and on a
  trickling one, where interrupts land in the middle of a block).
* fan-out over three loopback addresses (12 connections) → byte-exact, and the
  summary reports three edges.
* one of the two given edges refuses connections → retired → byte-exact result.
* missing object → clean failure.

Against the real mirror: full downloads at 96 connections over 12–19 edges, all
verified with `xz -t` (xz's own CRC64 over the whole archive) plus `sha256sum`
compared across an interrupted+resumed run.

### Bugs found and fixed while doing this

* **Block-completion loop never terminated at EOF.** `marked_upto` reached the
  file size and the loop kept re-marking the last block, spinning a core
  forever: *every* download hung at 100 % instead of finalising (the previous
  code had the same loop, which is why old runs only ever finished when killed).
  Now bounded by both the write position and the file size.
* **Blocks were marked done before their last byte was on disk**, so an
  interrupt could "claim" a half-written block and resume produced a corrupt
  file. Caught by the trickling-server interrupt test; the invariant is now
  enforced and covered by a test.
* **Work was silently dropped when a worker gave up** (the rest of its chunk
  was never fetched, so the run reported incomplete). `WorkQueue::requeue()`
  puts it back.
* **Use-after-free in the TLS session cache**: `take_session()` handed out a
  borrowed `SSL_SESSION*` that another thread could free. Now refcounted.
* **`getaddrinfo()` per connection**: with 96 workers starting at once this
  serialises in the resolver. Edges are resolved once (in parallel) and handed
  to workers as `(ip, port, family)`.

## Known limitations / next steps

* No redirect following (deliberate: the probe tells you instead of silently
  downloading an HTML page).
* HTTP/1.1 only. HTTP/2 would not help against per-edge byte-rate limiting —
  multiplexing would put all streams on one limited pipe.
* No checksum verification (the server offers no digest header); `xz -t`/`gzip -t`
  or `sha256sum` are the practical checks.
* No proxy support; `--endpoints`/`--connect-to` cover mirror pinning.
* Blocks are marked done before `fsync`, so a hard power loss can claim a block
  that is not durable. `fsync` at the end is unconditional; periodic `fsync`
  could be added in `FileSink`.
* Fan-out uses only addresses the mirror's own DNS hands out — no scanning —
  but it is still several dozen sockets, so the defaults stay moderate (96) and
  the ramp stops at the first plateau.
* Leases are handed out to the *least loaded* edge, which is optimal while the
  edges behave alike. When they do not, weighting by measured per-edge rate
  (and moving a worker when its edge is far below the median) would squeeze out
  a few more percent; `EndpointPool` already measures the numbers for it.
