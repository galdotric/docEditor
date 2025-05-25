# === Compiler and Flags ===
CC = gcc

# Base flags
BASE_CFLAGS = -std=c11 -Wall -Wextra -Wno-unused-parameter -g -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L -Ilibs -fPIC -Wvla
BASE_LDFLAGS = -pthread -fPIC -g

# Default (ASan-enabled) flags
CFLAGS ?= $(BASE_CFLAGS) -fno-sanitize=all
LDFLAGS ?= $(BASE_LDFLAGS) -fno-sanitize=all -fsanitize=address

# === Directories ===
SRC_DIR = source
LIB_DIR = libs
OBJ_DIR = obj

# === Source Files ===
SERVER_SRC = $(SRC_DIR)/server.c $(SRC_DIR)/markdown.c
CLIENT_SRC = $(SRC_DIR)/client.c $(SRC_DIR)/markdown.c

# === Object Files ===
SERVER_OBJ = $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SERVER_SRC))
CLIENT_OBJ = $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(CLIENT_SRC))

# === Executables ===
SERVER = server
CLIENT = client

# === Headers ===
HEADERS = $(LIB_DIR)/markdown.h $(LIB_DIR)/document.h

# === Default Target ===
all: $(SERVER) $(CLIENT)

# === Object Directory ===
$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

# === Build markdown.o explicitly ===
$(OBJ_DIR)/markdown.o: $(SRC_DIR)/markdown.c $(HEADERS) | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# === Create root-level markdown.o for test compatibility ===
markdown.o: $(OBJ_DIR)/markdown.o
	cp $< $@

# === Server Build ===
$(SERVER): $(OBJ_DIR) $(SERVER_OBJ)
	$(CC) $(SERVER_OBJ) -o $@ $(LDFLAGS)

# === Client Build ===
$(CLIENT): $(OBJ_DIR) $(CLIENT_OBJ)
	$(CC) $(CLIENT_OBJ) -o $@ $(LDFLAGS)

# === Pattern Rule for Compiling .c to .o ===
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c $(HEADERS) | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# === Clean Target ===
clean:
	rm -rf $(OBJ_DIR) $(SERVER) $(CLIENT) *.o

# === Valgrind Target (no ASan) ===
valgrind: clean
	$(MAKE) CFLAGS="$(BASE_CFLAGS)" LDFLAGS="$(BASE_LDFLAGS)"

# === ASan Target (explicit sanitizer rebuild) ===
asan: clean
	$(MAKE) CFLAGS="$(BASE_CFLAGS) -fno-sanitize=all" LDFLAGS="$(BASE_LDFLAGS) -fno-sanitize=all -fsanitize=address"

.PHONY: all clean valgrind asan
