CC ?= cc
CFLAGS ?= -std=c17 -O0 -g -Wall -Wextra -Wpedantic -Wconversion -Wshadow
LDFLAGS ?= -pthread
SAN_FLAGS ?= -fsanitize=address,undefined -fno-omit-frame-pointer

SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)
BIN := bin/mini-kv

# Unit test suites (meaningfully named)
TEST_CRUD_SRC := tests/test_crud_operations.c src/kv.c
TEST_CRUD_BIN := bin/test_crud_operations
TEST_CRUD_VAL := bin/test_crud_operations_valgrind

TEST_MEM_SRC := tests/test_memory_tracking.c src/kv.c
TEST_MEM_BIN := bin/test_memory_tracking
TEST_MEM_VAL := bin/test_memory_tracking_valgrind

TEST_RESIZE_SRC := tests/test_hash_table_resize.c src/kv.c
TEST_RESIZE_BIN := bin/test_hash_table_resize
TEST_RESIZE_VAL := bin/test_hash_table_resize_valgrind

TEST_PERSIST_SRC := tests/test_persistence.c src/kv.c
TEST_PERSIST_BIN := bin/test_persistence
TEST_PERSIST_VAL := bin/test_persistence_valgrind

TEST_BINS := $(TEST_CRUD_BIN) $(TEST_MEM_BIN) $(TEST_RESIZE_BIN) $(TEST_PERSIST_BIN)
VAL_BINS := $(TEST_CRUD_VAL) $(TEST_MEM_VAL) $(TEST_RESIZE_VAL) $(TEST_PERSIST_VAL)

all: $(BIN)

$(BIN): $(OBJ)
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

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

test: $(TEST_BINS)
	./$(TEST_CRUD_BIN)
	./$(TEST_MEM_BIN)
	./$(TEST_RESIZE_BIN)
	./$(TEST_PERSIST_BIN)

valgrind: $(VAL_BINS)
	valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 ./$(TEST_CRUD_VAL)
	valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 ./$(TEST_MEM_VAL)
	valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 ./$(TEST_RESIZE_VAL)
	valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 ./$(TEST_PERSIST_VAL)

asan: CFLAGS += $(SAN_FLAGS)
asan: LDFLAGS += $(SAN_FLAGS)
asan: clean all

ubsan: CFLAGS += -fsanitize=undefined -fno-omit-frame-pointer
ubsan: LDFLAGS += -fsanitize=undefined
ubsan: clean all

clean:
	rm -f $(OBJ) $(BIN) $(TEST_BINS) $(VAL_BINS) bin/test_* *.log *.aof tests/*.log tests/*.aof data/*.log data/*.aof

.PHONY: all test valgrind asan ubsan clean
