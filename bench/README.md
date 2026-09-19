# Benchmarks

Not built by default — they are only meaningful in an optimized build:

```bash
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release -DBUILD_SLICK_NET_BENCH=ON \
      -DBUILD_SLICK_NET_TESTS=OFF -DBUILD_SLICK_NET_EXAMPLES=OFF
cmake --build build-bench -j
./build-bench/bench/write_chain_bench
```

## `write_chain_bench`

A/Bs the two wakeup paths `Websocket<>::send()` has had:

| path   | what it does                                                                       |
| ------ | ---------------------------------------------------------------------------------- |
| `post` | the path before `write_chain_gate` — every send posts a wakeup, and the handler CASes a flag, so wakeups landing on a running chain cost a post and do nothing |
| `gate` | the current path — a `seq_cst` fence and a load decide whether a chain is already running, and only the send that finds none posts |

Both run against the same `slick::queue`, the same `io_context` and the same consumer. The chain
drains one record per turn and re-enters through the `io_context`, the way `on_write()` re-enters
`do_write()`, with `--write-ns` standing in for the socket write. So what is under test is one
fence per send against one post per send, and nothing else.

**This is a microbenchmark.** No socket, no TLS, no beast write path, none of the kernel time a
live connection spends. It bounds the wakeup decision, not end-to-end WebSocket throughput.

### Options

| option         | default | meaning                                      |
| -------------- | ------- | -------------------------------------------- |
| `--producers`  | 4       | sending threads                               |
| `--records`    | 50000   | records per producer                          |
| `--payload`    | 64      | payload bytes per record                      |
| `--write-ns`   | 250     | simulated per-record consumer work            |
| `--gap-ns`     | 0       | producer think time between sends             |
| `--reps`       | 5       | repetitions per path, reported as the median  |
| `--timeout-s`  | 60      | per-run stall timeout                         |

`--gap-ns` is the knob that matters. It sets how much sends overlap a running chain, which is what
decides whether the gate has any posts to save.

### Results, 16-thread x86-64 desktop, MSVC Release

One producer, `--write-ns 250`, median of 3–5 reps. "posts" is how many wakeups the path issued
for the whole run:

| gap between sends | `post` ns/send | posts  | `gate` ns/send | posts | gate saves   |
| ----------------- | -------------- | ------ | -------------- | ----- | ------------ |
| 0 (burst)         | 1850           | 20000  | 43             | 1     | 1808 ns/send |
| 1 µs              | 2939           | 20000  | 1106           | 1     | 1833 ns/send |
| 2 µs              | 3991           | 10000  | 2138           | 1     | 1853 ns/send |
| 5 µs              | 7602           | 10000  | 6200           | 2149  | 1402 ns/send |
| 10 µs             | 14015          | 5000   | 13423          | 2521  | 592 ns/send  |
| 20 µs             | 24749          | 5000   | 24672          | 4868  | 78 ns/send   |
| 50 µs             | 55120          | 3000   | 55175          | 3000  | nothing      |

At four producers bursting (the default config) the whole run goes from 1046 ms to 645 ms —
**1.6× wall time**, with 199,999 of 200,000 posts removed.

**Reading it.** The fence did not show up as a cost anywhere. Where sends overlap a running chain
the gate replaces a ~1.8 µs post with a ~40 ns fence-and-load. Where they never overlap (50 µs
apart) the gate posts on every send exactly like the old path, and the difference between the two
disappears into run-to-run noise — repeat runs of that case gave +39, +6, +947 and +386 ns, all
positive, all well inside the spread. So the fence costs less than this can resolve, which is
under ~100 ns against a 55 µs send interval.

The regime where the fence would be expensive — a very high send rate — is the same regime where
sends pile onto a running chain and the gate has the most posts to remove. That is why there is no
crossover in the table: the two effects scale together.

**Before relying on these numbers, run it on your target hardware.** One desktop is not a claim
about your machine, and the socket this benchmark leaves out is the biggest cost in a real send.
