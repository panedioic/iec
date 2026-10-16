# IEC — client call interception skeleton
#
#   make            build the shim (per MODE) + the server
#   make run        run weston-terminal once with interception (server must already be running)
#   make run-server run both servers in the foreground (each service on its own thread)
#   make clean
#
# MODE=warp (default)  link-time interception: shim becomes build/shim_tty.o, relinked with the
#                     client via -Wl,--wrap=forkpty, producing dist/weston-terminal-wrap
# MODE=preload       load-time interception: shim becomes dist/libiec.so, injected into the
#                    stock ./weston-terminal via LD_PRELOAD
#
# Both modes must also include common/sock_client.c and common/rpc.c: the shim goes over UDS to
# find the server and does the round trip with helpers shared by the two shims, so these symbols
# must be compiled in (an LD_PRELOAD .so can't expect the host to provide them).
#
# Typical usage (two terminals):
#   $ make run-server
#   $ make run

MODE ?= warp
WESTON_SRC ?= /workspace/dev/weston

DIST  := dist
BUILD := build

CFLAGS   += -Wall -Icommon -D_GNU_SOURCE
CXXFLAGS += -Wall -Icommon -D_GNU_SOURCE
SHIM_SRC := tty/shim.c
SHIM_HDR := common/intercept.h common/rpc.h common/sock_operation.h
EXEC_SRC := exec/shim.c
EXEC_HDR := common/intercept.h common/rpc.h
SOCK_SRC := common/sock_client.c
SOCK_HDR := common/sock_client.h
RPC_SRC  := common/rpc.c
RPC_HDR  := common/rpc.h

# Every entry point the shims take over. MODE=warp needs a --wrap for each, so
# that calls from the client are redirected to __wrap_<name>; MODE=preload
# interposes them by symbol name instead and needs none of these.
SHIM_WRAPS := open close dup dup2 dup3 fcntl ioctl isatty ttyname \
              ttyname_r tcgetattr tcsetattr tcgetpgrp tcsetpgrp getpt grantpt \
              unlockpt ptsname openpty login_tty forkpty \
              execve execv execvp execvpe execl execlp execle
WRAP_FLAGS := $(foreach s,$(SHIM_WRAPS),-Wl,--wrap=$(s))

SERVER_BIN  := $(DIST)/iec_server
SERVER_SRCS := main.cpp tty/tty_server.cpp exec/exec_server.cpp exec/exec_so.cpp \
               exec/exec_qemu.cpp exec/elf.cpp common/sock_server.cpp
SERVER_HDR  := tty/tty_server.h exec/exec_server.h exec/exec_so.h exec/exec_types.h \
               exec/exec_qemu.h exec/elf.h common/sock_server.h common/sock_operation.h

ifeq ($(MODE),warp)
  TARGET := $(DIST)/weston-terminal-wrap
  RUN    := ./$(TARGET)
else ifeq ($(MODE),preload)
  TARGET := $(DIST)/libiec.so
  RUN    := LD_PRELOAD=./$(TARGET) ./weston-terminal
else
  $(error MODE must be warp or preload, got "$(MODE)")
endif

.PHONY: all run server run-server clean

all: $(TARGET) server

$(DIST) $(BUILD):
	mkdir -p $@

# ── server ────────────────────────────────────────────────
server: $(SERVER_BIN)

$(SERVER_BIN): $(SERVER_SRCS) $(SERVER_HDR) | $(DIST)
	$(CXX) $(CXXFLAGS) -pthread $(SERVER_SRCS) -o $@ -ldl

run-server: $(SERVER_BIN)
	./$(SERVER_BIN)

# ── shim (both modes share the same source) ────────────────
$(BUILD)/shim_tty.o: $(SHIM_SRC) $(SHIM_HDR) | $(BUILD)
	$(CC) $(CFLAGS) -DINT_WARP -fPIC -c $< -o $@

$(BUILD)/exec_shim.o: $(EXEC_SRC) $(EXEC_HDR) | $(BUILD)
	$(CC) $(CFLAGS) -DINT_WARP -fPIC -c $< -o $@

$(BUILD)/sock_client.o: $(SOCK_SRC) $(SOCK_HDR) | $(BUILD)
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

$(BUILD)/rpc.o: $(RPC_SRC) $(RPC_HDR) $(SOCK_HDR) | $(BUILD)
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

$(DIST)/libiec.so: $(SHIM_SRC) $(EXEC_SRC) $(RPC_SRC) $(SHIM_HDR) $(EXEC_HDR) $(RPC_HDR) $(SOCK_SRC) $(SOCK_HDR) | $(DIST)
	$(CC) $(CFLAGS) -DINT_PRELOAD -shared -fPIC $(SHIM_SRC) $(EXEC_SRC) $(RPC_SRC) $(SOCK_SRC) -o $@ -ldl

# --wrap only takes effect at link time and does nothing for an already-built executable, so the shim must be relinked together with the client
$(DIST)/weston-terminal-wrap: $(BUILD)/shim_tty.o $(BUILD)/exec_shim.o $(BUILD)/sock_client.o $(BUILD)/rpc.o scripts/build_client.sh | $(DIST)
	WESTON_SRC=$(WESTON_SRC) scripts/build_client.sh -o $@ \
		$(WRAP_FLAGS) $(BUILD)/shim_tty.o $(BUILD)/exec_shim.o $(BUILD)/sock_client.o $(BUILD)/rpc.o

run: all
	$(RUN)

clean:
	rm -f $(DIST)/libiec.so $(DIST)/weston-terminal-wrap $(SERVER_BIN)
	rm -rf $(BUILD)/shim_tty.o $(BUILD)/exec_shim.o $(BUILD)/sock_client.o $(BUILD)/rpc.o $(BUILD)/weston-term
