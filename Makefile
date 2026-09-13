CC ?= cc
CFLAGS ?= -std=c17 -O0 -g -Wall -Wextra -Wpedantic -Wconversion -Wshadow
LDFLAGS ?= -pthread
SAN_FLAGS ?= -fsanitize=address,undefined -fno-omit-frame-pointer

MODULAR_LIB_SRC := src/kv.c src/aof.c src/eviction.c src/list.c src/set.c src/snapshot.c src/replication.c
SERVER_LIB_SRC := $(MODULAR_LIB_SRC) src/server.c src/protocol.c src/thread_pool.c
SERVER_SRC := src/main.c $(SERVER_LIB_SRC)
SERVER_OBJ := $(SERVER_SRC:.c=.o)
BIN := bin/mini-kv
BENCH_BIN := bin/mini-kv-benchmark

# Unit test suites (meaningfully named)
TEST_CRUD_SRC := tests/test_crud_operations.c $(MODULAR_LIB_SRC)
TEST_CRUD_BIN := bin/test_crud_operations
TEST_CRUD_VAL := bin/test_crud_operations_valgrind

TEST_MEM_SRC := tests/test_memory_tracking.c $(MODULAR_LIB_SRC)
TEST_MEM_BIN := bin/test_memory_tracking
TEST_MEM_VAL := bin/test_memory_tracking_valgrind

TEST_RESIZE_SRC := tests/test_hash_table_resize.c $(MODULAR_LIB_SRC)
TEST_RESIZE_BIN := bin/test_hash_table_resize
TEST_RESIZE_VAL := bin/test_hash_table_resize_valgrind

TEST_PERSIST_SRC := tests/test_persistence.c $(MODULAR_LIB_SRC)
TEST_PERSIST_BIN := bin/test_persistence
TEST_PERSIST_VAL := bin/test_persistence_valgrind

TEST_TCP_SRC := tests/test_tcp_server.c $(SERVER_LIB_SRC)
TEST_TCP_BIN := bin/test_tcp_server
TEST_TCP_VAL := bin/test_tcp_server_valgrind

TEST_CONC_SRC := tests/test_concurrency.c $(MODULAR_LIB_SRC)
TEST_CONC_BIN := bin/test_concurrency
TEST_CONC_VAL := bin/test_concurrency_valgrind

TEST_TP_SRC := tests/test_thread_pool.c src/thread_pool.c
TEST_TP_BIN := bin/test_thread_pool
TEST_TP_VAL := bin/test_thread_pool_valgrind

TEST_TTL_SRC := tests/test_ttl_eviction.c $(MODULAR_LIB_SRC)
TEST_TTL_BIN := bin/test_ttl_eviction
TEST_TTL_VAL := bin/test_ttl_eviction_valgrind

TEST_SNAPSHOT_SRC := tests/test_snapshot.c $(MODULAR_LIB_SRC)
TEST_SNAPSHOT_BIN := bin/test_snapshot
TEST_SNAPSHOT_VAL := bin/test_snapshot_valgrind

TEST_REWRITE_SRC := tests/test_aof_rewrite.c $(MODULAR_LIB_SRC)
TEST_REWRITE_BIN := bin/test_aof_rewrite
TEST_REWRITE_VAL := bin/test_aof_rewrite_valgrind

TEST_DATA_STRUCTURES_SRC := tests/test_data_structures.c $(MODULAR_LIB_SRC)
TEST_DATA_STRUCTURES_BIN := bin/test_data_structures
TEST_DATA_STRUCTURES_VAL := bin/test_data_structures_valgrind

TEST_REPLICATION_SRC := tests/test_replication.c $(SERVER_LIB_SRC)
TEST_REPLICATION_BIN := bin/test_replication
TEST_REPLICATION_VAL := bin/test_replication_valgrind

TEST_BINS := $(TEST_CRUD_BIN) $(TEST_MEM_BIN) $(TEST_RESIZE_BIN) $(TEST_PERSIST_BIN) $(TEST_TCP_BIN) $(TEST_CONC_BIN) $(TEST_TP_BIN) $(TEST_TTL_BIN) $(TEST_SNAPSHOT_BIN) $(TEST_REWRITE_BIN) $(TEST_DATA_STRUCTURES_BIN) $(TEST_REPLICATION_BIN)
VAL_BINS := $(TEST_CRUD_VAL) $(TEST_MEM_VAL) $(TEST_RESIZE_VAL) $(TEST_PERSIST_VAL) $(TEST_TCP_VAL) $(TEST_CONC_VAL) $(TEST_TP_VAL) $(TEST_TTL_VAL) $(TEST_SNAPSHOT_VAL) $(TEST_REWRITE_VAL) $(TEST_DATA_STRUCTURES_VAL) $(TEST_REPLICATION_VAL)

# ThreadSanitizer (TSan) configuration
TSAN_FLAGS ?= -fsanitize=thread -fPIE -pie -g -O1
TEST_CRUD_TSAN := bin/test_crud_operations_tsan
TEST_MEM_TSAN := bin/test_memory_tracking_tsan
TEST_RESIZE_TSAN := bin/test_hash_table_resize_tsan
TEST_PERSIST_TSAN := bin/test_persistence_tsan
TEST_TCP_TSAN := bin/test_tcp_server_tsan
TEST_CONC_TSAN := bin/test_concurrency_tsan
TEST_TP_TSAN := bin/test_thread_pool_tsan
TEST_TTL_TSAN := bin/test_ttl_eviction_tsan
TEST_SNAPSHOT_TSAN := bin/test_snapshot_tsan
TEST_REWRITE_TSAN := bin/test_aof_rewrite_tsan
TEST_DATA_STRUCTURES_TSAN := bin/test_data_structures_tsan
TEST_REPLICATION_TSAN := bin/test_replication_tsan
TSAN_BINS := $(TEST_CRUD_TSAN) $(TEST_MEM_TSAN) $(TEST_RESIZE_TSAN) $(TEST_PERSIST_TSAN) $(TEST_TCP_TSAN) $(TEST_CONC_TSAN) $(TEST_TP_TSAN) $(TEST_TTL_TSAN) $(TEST_SNAPSHOT_TSAN) $(TEST_REWRITE_TSAN) $(TEST_DATA_STRUCTURES_TSAN) $(TEST_REPLICATION_TSAN)

all: $(BIN) $(BENCH_BIN)

$(BIN): $(SERVER_OBJ)
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BENCH_BIN): src/benchmark.c
	@mkdir -p bin
	$(CC) $(CFLAGS) -Iinclude -o $@ $< $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -Iinclude -c $< -o $@

# ASan / UBSan test binaries
$(TEST_CRUD_BIN): $(TEST_CRUD_SRC) include/kv.h
	@mkdir -p bin
	$(CC) $(CFLAGS) $(SAN_FLAGS) -Iinclude $(TEST_CRUD_SRC) -o $@ $(LDFLAGS) $(SAN_FLAGS)

$(TEST_MEM_BIN): $(TEST_MEM_SRC) include/kv.h
	@mkdir -p bin
	$(CC) $(CFLAGS) $(SAN_FLAGS) -Iinclude $(TEST_MEM_SRC) -o $@ $(LDFLAGS) $(SAN_FLAGS)

$(TEST_RESIZE_BIN): $(TEST_RESIZE_SRC) include/kv.h
	@mkdir -p bin
	$(CC) $(CFLAGS) $(SAN_FLAGS) -Iinclude $(TEST_RESIZE_SRC) -o $@ $(LDFLAGS) $(SAN_FLAGS)

$(TEST_PERSIST_BIN): $(TEST_PERSIST_SRC) include/kv.h
	@mkdir -p bin
	$(CC) $(CFLAGS) $(SAN_FLAGS) -Iinclude $(TEST_PERSIST_SRC) -o $@ $(LDFLAGS) $(SAN_FLAGS)

$(TEST_TCP_BIN): $(TEST_TCP_SRC) include/kv.h include/server.h include/thread_pool.h include/protocol.h
	@mkdir -p bin
	$(CC) $(CFLAGS) $(SAN_FLAGS) -Iinclude $(TEST_TCP_SRC) -o $@ $(LDFLAGS) $(SAN_FLAGS)

$(TEST_CONC_BIN): $(TEST_CONC_SRC) include/kv.h
	@mkdir -p bin
	$(CC) $(CFLAGS) $(SAN_FLAGS) -Iinclude $(TEST_CONC_SRC) -o $@ $(LDFLAGS) $(SAN_FLAGS)

$(TEST_TP_BIN): $(TEST_TP_SRC) include/thread_pool.h include/kv.h
	@mkdir -p bin
	$(CC) $(CFLAGS) $(SAN_FLAGS) -Iinclude $(TEST_TP_SRC) -o $@ $(LDFLAGS) $(SAN_FLAGS)

$(TEST_TTL_BIN): $(TEST_TTL_SRC) include/kv.h include/thread_pool.h
	@mkdir -p bin
	$(CC) $(CFLAGS) $(SAN_FLAGS) -Iinclude $(TEST_TTL_SRC) -o $@ $(LDFLAGS) $(SAN_FLAGS)

$(TEST_SNAPSHOT_BIN): $(TEST_SNAPSHOT_SRC) include/kv.h include/thread_pool.h
	@mkdir -p bin
	$(CC) $(CFLAGS) $(SAN_FLAGS) -Iinclude $(TEST_SNAPSHOT_SRC) -o $@ $(LDFLAGS) $(SAN_FLAGS)

$(TEST_REWRITE_BIN): $(TEST_REWRITE_SRC) include/kv.h include/thread_pool.h
	@mkdir -p bin
	$(CC) $(CFLAGS) $(SAN_FLAGS) -Iinclude $(TEST_REWRITE_SRC) -o $@ $(LDFLAGS) $(SAN_FLAGS)

$(TEST_DATA_STRUCTURES_BIN): $(TEST_DATA_STRUCTURES_SRC) include/kv.h
	@mkdir -p bin
	$(CC) $(CFLAGS) $(SAN_FLAGS) -Iinclude $(TEST_DATA_STRUCTURES_SRC) -o $@ $(LDFLAGS) $(SAN_FLAGS)

$(TEST_REPLICATION_BIN): $(TEST_REPLICATION_SRC) include/kv.h include/replication.h include/server.h include/protocol.h
	@mkdir -p bin
	$(CC) $(CFLAGS) $(SAN_FLAGS) -Iinclude $(TEST_REPLICATION_SRC) -o $@ $(LDFLAGS) $(SAN_FLAGS)

# Valgrind test binaries (clean, non-sanitized)
$(TEST_CRUD_VAL): $(TEST_CRUD_SRC) include/kv.h
	@mkdir -p bin
	$(CC) $(CFLAGS) -Iinclude $(TEST_CRUD_SRC) -o $@ $(LDFLAGS)

$(TEST_MEM_VAL): $(TEST_MEM_SRC) include/kv.h
	@mkdir -p bin
	$(CC) $(CFLAGS) -Iinclude $(TEST_MEM_SRC) -o $@ $(LDFLAGS)

$(TEST_RESIZE_VAL): $(TEST_RESIZE_SRC) include/kv.h
	@mkdir -p bin
	$(CC) $(CFLAGS) -Iinclude $(TEST_RESIZE_SRC) -o $@ $(LDFLAGS)

$(TEST_PERSIST_VAL): $(TEST_PERSIST_SRC) include/kv.h
	@mkdir -p bin
	$(CC) $(CFLAGS) -Iinclude $(TEST_PERSIST_SRC) -o $@ $(LDFLAGS)

$(TEST_TCP_VAL): $(TEST_TCP_SRC) include/kv.h include/server.h include/thread_pool.h include/protocol.h
	@mkdir -p bin
	$(CC) $(CFLAGS) -Iinclude $(TEST_TCP_SRC) -o $@ $(LDFLAGS)

$(TEST_CONC_VAL): $(TEST_CONC_SRC) include/kv.h
	@mkdir -p bin
	$(CC) $(CFLAGS) -Iinclude $(TEST_CONC_SRC) -o $@ $(LDFLAGS)

$(TEST_TP_VAL): $(TEST_TP_SRC) include/thread_pool.h include/kv.h
	@mkdir -p bin
	$(CC) $(CFLAGS) -Iinclude $(TEST_TP_SRC) -o $@ $(LDFLAGS)

$(TEST_TTL_VAL): $(TEST_TTL_SRC) include/kv.h include/thread_pool.h
	@mkdir -p bin
	$(CC) $(CFLAGS) -Iinclude $(TEST_TTL_SRC) -o $@ $(LDFLAGS)

$(TEST_SNAPSHOT_VAL): $(TEST_SNAPSHOT_SRC) include/kv.h include/thread_pool.h
	@mkdir -p bin
	$(CC) $(CFLAGS) -Iinclude $(TEST_SNAPSHOT_SRC) -o $@ $(LDFLAGS)

$(TEST_REWRITE_VAL): $(TEST_REWRITE_SRC) include/kv.h include/thread_pool.h
	@mkdir -p bin
	$(CC) $(CFLAGS) -Iinclude $(TEST_REWRITE_SRC) -o $@ $(LDFLAGS)

$(TEST_DATA_STRUCTURES_VAL): $(TEST_DATA_STRUCTURES_SRC) include/kv.h
	@mkdir -p bin
	$(CC) $(CFLAGS) -Iinclude $(TEST_DATA_STRUCTURES_SRC) -o $@ $(LDFLAGS)

# ThreadSanitizer test binaries
$(TEST_CRUD_TSAN): $(TEST_CRUD_SRC) include/kv.h
	@mkdir -p bin
	$(CC) -std=c17 $(TSAN_FLAGS) -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Iinclude $(TEST_CRUD_SRC) -o $@ $(LDFLAGS) $(TSAN_FLAGS)

$(TEST_MEM_TSAN): $(TEST_MEM_SRC) include/kv.h
	@mkdir -p bin
	$(CC) -std=c17 $(TSAN_FLAGS) -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Iinclude $(TEST_MEM_SRC) -o $@ $(LDFLAGS) $(TSAN_FLAGS)

$(TEST_RESIZE_TSAN): $(TEST_RESIZE_SRC) include/kv.h
	@mkdir -p bin
	$(CC) -std=c17 $(TSAN_FLAGS) -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Iinclude $(TEST_RESIZE_SRC) -o $@ $(LDFLAGS) $(TSAN_FLAGS)

$(TEST_PERSIST_TSAN): $(TEST_PERSIST_SRC) include/kv.h
	@mkdir -p bin
	$(CC) -std=c17 $(TSAN_FLAGS) -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Iinclude $(TEST_PERSIST_SRC) -o $@ $(LDFLAGS) $(TSAN_FLAGS)

$(TEST_TCP_TSAN): $(TEST_TCP_SRC) include/kv.h include/server.h include/thread_pool.h include/protocol.h
	@mkdir -p bin
	$(CC) -std=c17 $(TSAN_FLAGS) -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Iinclude $(TEST_TCP_SRC) -o $@ $(LDFLAGS) $(TSAN_FLAGS)

$(TEST_CONC_TSAN): $(TEST_CONC_SRC) include/kv.h
	@mkdir -p bin
	$(CC) -std=c17 $(TSAN_FLAGS) -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Iinclude $(TEST_CONC_SRC) -o $@ $(LDFLAGS) $(TSAN_FLAGS)

$(TEST_TP_TSAN): $(TEST_TP_SRC) include/thread_pool.h include/kv.h
	@mkdir -p bin
	$(CC) -std=c17 $(TSAN_FLAGS) -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Iinclude $(TEST_TP_SRC) -o $@ $(LDFLAGS) $(TSAN_FLAGS)

$(TEST_TTL_TSAN): $(TEST_TTL_SRC) include/kv.h include/thread_pool.h
	@mkdir -p bin
	$(CC) -std=c17 $(TSAN_FLAGS) -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Iinclude $(TEST_TTL_SRC) -o $@ $(LDFLAGS) $(TSAN_FLAGS)

$(TEST_SNAPSHOT_TSAN): $(TEST_SNAPSHOT_SRC) include/kv.h include/thread_pool.h
	@mkdir -p bin
	$(CC) -std=c17 $(TSAN_FLAGS) -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Iinclude $(TEST_SNAPSHOT_SRC) -o $@ $(LDFLAGS) $(TSAN_FLAGS)

$(TEST_REWRITE_TSAN): $(TEST_REWRITE_SRC) include/kv.h include/thread_pool.h
	@mkdir -p bin
	$(CC) -std=c17 $(TSAN_FLAGS) -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Iinclude $(TEST_REWRITE_SRC) -o $@ $(LDFLAGS) $(TSAN_FLAGS)

$(TEST_DATA_STRUCTURES_TSAN): $(TEST_DATA_STRUCTURES_SRC) include/kv.h
	@mkdir -p bin
	$(CC) -std=c17 $(TSAN_FLAGS) -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Iinclude $(TEST_DATA_STRUCTURES_SRC) -o $@ $(LDFLAGS) $(TSAN_FLAGS)

test: $(TEST_BINS)
	./$(TEST_CRUD_BIN)
	./$(TEST_MEM_BIN)
	./$(TEST_RESIZE_BIN)
	./$(TEST_PERSIST_BIN)
	./$(TEST_TCP_BIN)
	./$(TEST_CONC_BIN)
	./$(TEST_TP_BIN)
	./$(TEST_TTL_BIN)
	./$(TEST_SNAPSHOT_BIN)
	./$(TEST_REWRITE_BIN)
	./$(TEST_DATA_STRUCTURES_BIN)
	./$(TEST_REPLICATION_BIN)

valgrind: $(VAL_BINS)
	valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 ./$(TEST_CRUD_VAL)
	valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 ./$(TEST_MEM_VAL)
	valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 ./$(TEST_RESIZE_VAL)
	valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 ./$(TEST_PERSIST_VAL)
	valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 ./$(TEST_TCP_VAL)
	valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 ./$(TEST_CONC_VAL)
	valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 ./$(TEST_TP_VAL)
	valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 ./$(TEST_TTL_VAL)
	valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 ./$(TEST_SNAPSHOT_VAL)
	valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 ./$(TEST_REWRITE_VAL)
	valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 ./$(TEST_DATA_STRUCTURES_VAL)

tsan: $(TSAN_BINS)
	./$(TEST_CRUD_TSAN)
	./$(TEST_MEM_TSAN)
	./$(TEST_RESIZE_TSAN)
	./$(TEST_PERSIST_TSAN)
	./$(TEST_TCP_TSAN)
	./$(TEST_CONC_TSAN)
	./$(TEST_TP_TSAN)
	./$(TEST_TTL_TSAN)
	./$(TEST_SNAPSHOT_TSAN)
	./$(TEST_REWRITE_TSAN)
	./$(TEST_DATA_STRUCTURES_TSAN)

asan: CFLAGS += $(SAN_FLAGS)
asan: LDFLAGS += $(SAN_FLAGS)
asan: clean all

ubsan: CFLAGS += -fsanitize=undefined -fno-omit-frame-pointer
ubsan: LDFLAGS += -fsanitize=undefined
ubsan: clean all

clean:
	rm -f $(SERVER_OBJ) $(BIN) $(BENCH_BIN) $(TEST_BINS) $(VAL_BINS) $(TSAN_BINS) bin/test_* *.log *.aof tests/*.log tests/*.aof data/*.log data/*.aof test_concurrency.aof *.snap *.snap.tmp

.PHONY: all test valgrind asan ubsan tsan clean
