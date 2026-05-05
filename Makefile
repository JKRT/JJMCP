CXX ?= c++
PKG_CONFIG ?= pkg-config
INSTALL ?= install
PREFIX ?= /usr/local
DESTDIR ?=
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/share/man
DOCDIR ?= $(PREFIX)/share/doc/jjmcp

BUILD_DIR := build
SRC_DIR := src
TEST_DIR := tests
MAN_DIR := man

CPPFLAGS += -I$(SRC_DIR)
CXXFLAGS += -std=c++23 -Wall -Wextra -Wpedantic -O2
LDLIBS +=

JSON_CFLAGS := $(shell $(PKG_CONFIG) --cflags nlohmann_json 2>/dev/null)
JSON_LIBS := $(shell $(PKG_CONFIG) --libs nlohmann_json 2>/dev/null)

CPPFLAGS += $(JSON_CFLAGS)
LDLIBS += $(JSON_LIBS)

LIB_SRCS := \
	$(SRC_DIR)/julia_wrap.cpp \
	$(SRC_DIR)/mcp.cpp \
	$(SRC_DIR)/process.cpp \
	$(SRC_DIR)/state.cpp \
	$(SRC_DIR)/tmux.cpp \
	$(SRC_DIR)/tools.cpp

APP_SRCS := $(SRC_DIR)/main.cpp $(LIB_SRCS)
TEST_SRCS := $(TEST_DIR)/test_main.cpp $(LIB_SRCS)

APP_OBJS := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(APP_SRCS))
TEST_OBJS := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(TEST_SRCS))

.PHONY: all test clean check-deps install uninstall help

all: $(BUILD_DIR)/jjmcp

help:
	@printf '%s\n' \
		'Targets:' \
		'  make          Build build/jjmcp' \
		'  make test     Build and run unit tests' \
		'  make install  Install jjmcp, man pages, and docs' \
		'  make clean    Remove build outputs' \
		'  make check-deps  Check compiler, pkg-config, nlohmann_json, and tmux' \
		'' \
		'Variables:' \
		'  PREFIX=/usr/local DESTDIR= BINDIR=$(PREFIX)/bin MANDIR=$(PREFIX)/share/man DOCDIR=$(PREFIX)/share/doc/jjmcp'

check-deps:
	@$(CXX) --version >/dev/null
	@$(PKG_CONFIG) --exists nlohmann_json
	@command -v tmux >/dev/null
	@printf 'Dependencies available.\n'

$(BUILD_DIR)/jjmcp: $(APP_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD_DIR)/jjmcp-tests: $(TEST_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

test: $(BUILD_DIR)/jjmcp-tests
	$(BUILD_DIR)/jjmcp-tests

install: $(BUILD_DIR)/jjmcp
	$(INSTALL) -d "$(DESTDIR)$(BINDIR)"
	$(INSTALL) -m 0755 "$(BUILD_DIR)/jjmcp" "$(DESTDIR)$(BINDIR)/jjmcp"
	$(INSTALL) -d "$(DESTDIR)$(MANDIR)/man1" "$(DESTDIR)$(MANDIR)/man5" "$(DESTDIR)$(MANDIR)/man7"
	$(INSTALL) -m 0644 "$(MAN_DIR)/jjmcp.1" "$(DESTDIR)$(MANDIR)/man1/jjmcp.1"
	$(INSTALL) -m 0644 "$(MAN_DIR)/jjmcp-config.5" "$(DESTDIR)$(MANDIR)/man5/jjmcp-config.5"
	$(INSTALL) -m 0644 "$(MAN_DIR)/jjmcp-security.7" "$(DESTDIR)$(MANDIR)/man7/jjmcp-security.7"
	$(INSTALL) -d "$(DESTDIR)$(DOCDIR)"
	$(INSTALL) -m 0644 README.md SECURITY.md docs/DESIGN.md "$(DESTDIR)$(DOCDIR)/"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/jjmcp"
	rm -f "$(DESTDIR)$(MANDIR)/man1/jjmcp.1"
	rm -f "$(DESTDIR)$(MANDIR)/man5/jjmcp-config.5"
	rm -f "$(DESTDIR)$(MANDIR)/man7/jjmcp-security.7"
	rm -rf "$(DESTDIR)$(DOCDIR)"

clean:
	rm -rf $(BUILD_DIR)

-include $(APP_OBJS:.o=.d)
-include $(TEST_OBJS:.o=.d)
