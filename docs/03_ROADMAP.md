# 03 — Build Roadmap

All phases are complete. Each phase ends with something that compiles, runs, and that you can explain.

| Phase | Goal | Deliverable | You should be able to explain | Status |
|---|---|---|---|---|
| **0** | Setup | Folder, Makefile, docs | What an LOB is; price-time priority; the worked example | ✅ |
| **1** | Domain types | `include/types.hpp` | Why prices are integer ticks (Q1) | ✅ |
| **2** | Data structures | `include/order_book.hpp` | Complexity table (Q9) | ✅ |
| **3** | Add + match + cancel | `src/order_book.cpp` | Matching loop (Q7); trades print at the resting price | ✅ |
| **4** | Tests | `tests/test_order_book.cpp` + `check_invariants()` | Every invariant and why it matters | ✅ |
| **5** | Reference model + fuzz | `tests/reference_book.hpp`, `tests/fuzz.cpp` | Differential testing (Q10) | ✅ |
| **6** | Modify + IOC/FOK | `modify()`, `TimeInForce` | Priority-loss rule on modify; FOK pre-check | ✅ |
| **7** | Replay | `apps/replay.cpp`, `src/csv.cpp`, `apps/gen_orders.cpp` | Parsing prices without `double`; I/O off the timed path | ✅ |
| **8** | Benchmark | `bench/bench.cpp` | Throughput vs latency percentiles; clock resolution | ✅ |
| **9** | Optimise | `FastOrderBook` (v2), `FlatIdMap` | Why it got faster (allocations, cache misses) | ✅ |
| **10** | Microstructure | `include/stats.hpp`, `apps/lobster_replay.cpp`, `analysis/plot_bbo.py` | Spread, microprice, imbalance; book reconstruction | ✅ |
| **11** | Polish | README with results, resume bullet, git | 2-minute project walkthrough | ✅ |

Commands: `make test`, `make fuzz`, `make bench`, `make plot`, `make run`.
