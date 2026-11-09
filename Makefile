# Build everything with plain make + clang++ (no CMake needed).
#
#   make            demo, replay, gen_orders, lobster_replay, bench (optimised)
#   make test       unit tests + quick fuzz + sample replays, all under ASan/UBSan
#   make fuzz       long differential fuzz (1M messages x 5 seeds per book), optimised
#   make bench      throughput + latency benchmark, v1 vs v2
#   make sample     generate data/generated_orders.csv and replay it with BBO output
#   make plot       plot the BBO/spread/imbalance time series (needs python3 + matplotlib)

CXX      := clang++
CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic -Iinclude -Itests
RELEASE  := -O3 -DNDEBUG
SANITIZE := -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined

BUILD   := build
LIB     := src/order_book.cpp src/fast_order_book.cpp src/csv.cpp
HEADERS := $(wildcard include/*.hpp tests/*.hpp)
APPS    := $(BUILD)/orderbook $(BUILD)/replay $(BUILD)/gen_orders $(BUILD)/lobster_replay $(BUILD)/bench

.PHONY: all test fuzz bench sample plot clean run

all: $(APPS)

$(BUILD):
	mkdir -p $(BUILD)

# --- optimised binaries ------------------------------------------------------
$(BUILD)/orderbook: src/main.cpp $(LIB) $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(RELEASE) $< $(LIB) -o $@

$(BUILD)/replay: apps/replay.cpp $(LIB) $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(RELEASE) $< $(LIB) -o $@

$(BUILD)/gen_orders: apps/gen_orders.cpp $(LIB) $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(RELEASE) $< $(LIB) -o $@

$(BUILD)/lobster_replay: apps/lobster_replay.cpp $(LIB) $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(RELEASE) $< $(LIB) -o $@

$(BUILD)/bench: bench/bench.cpp $(LIB) $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(RELEASE) $< $(LIB) -o $@

$(BUILD)/fuzz: tests/fuzz.cpp $(LIB) $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(RELEASE) $< $(LIB) -o $@

# --- sanitizer builds (tests) ---------------------------------------------------
$(BUILD)/tests_asan: tests/test_order_book.cpp $(LIB) $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(SANITIZE) $< $(LIB) -o $@

$(BUILD)/fuzz_asan: tests/fuzz.cpp $(LIB) $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(SANITIZE) $< $(LIB) -o $@

$(BUILD)/replay_asan: apps/replay.cpp $(LIB) $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(SANITIZE) $< $(LIB) -o $@

$(BUILD)/lobster_asan: apps/lobster_replay.cpp $(LIB) $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(SANITIZE) $< $(LIB) -o $@

# --- targets -----------------------------------------------------------------
test: $(BUILD)/tests_asan $(BUILD)/fuzz_asan $(BUILD)/replay_asan $(BUILD)/lobster_asan
	@echo "== unit tests =="
	./$(BUILD)/tests_asan
	@echo "\n== differential fuzz (quick) =="
	./$(BUILD)/fuzz_asan 30000 4
	@echo "\n== sample replay: v1 and v2 must produce identical trade tapes =="
	./$(BUILD)/replay_asan data/sample_orders.csv --book v1 --trades $(BUILD)/sample_trades_v1.csv > /dev/null
	./$(BUILD)/replay_asan data/sample_orders.csv --book v2 --trades $(BUILD)/sample_trades_v2.csv > /dev/null
	cmp $(BUILD)/sample_trades_v1.csv $(BUILD)/sample_trades_v2.csv
	cmp $(BUILD)/sample_trades_v2.csv data/sample_trades_expected.csv
	@echo "[pass]  sample replay matches expected trade tape"
	@echo "\n== LOBSTER sample: reconstructed book must match every snapshot =="
	./$(BUILD)/lobster_asan data/lobster_sample_message.csv data/lobster_sample_orderbook.csv | tee $(BUILD)/lobster.txt
	grep -q "(100.00%)" $(BUILD)/lobster.txt
	@echo "[pass]  LOBSTER sample reconstruction"
	@echo "\nALL TESTS PASSED"

fuzz: $(BUILD)/fuzz
	./$(BUILD)/fuzz 1000000 5

bench: $(BUILD)/bench
	./$(BUILD)/bench 2000000 42 --csv results/bench.csv

sample: $(BUILD)/gen_orders $(BUILD)/replay
	./$(BUILD)/gen_orders 200000 data/generated_orders.csv
	./$(BUILD)/replay data/generated_orders.csv --trades results/generated_trades.csv --bbo results/generated_bbo.csv

plot: sample
	python3 analysis/plot_bbo.py results/generated_bbo.csv results/bbo.png

run: $(BUILD)/orderbook
	./$(BUILD)/orderbook

clean:
	rm -rf $(BUILD)
