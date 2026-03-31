SOURCES = \
	src/pg_what_is_happening.c \
	src/shared_memory.c \
	src/plan_tree_walker.c \
	src/signal_handler.c \
	src/compatibility.c

ENVCFLAGS := $(CFLAGS)

OS_INFO := $(shell uname -rvom || echo "<no uname>")
COMMIT_HASH := $(shell git rev-parse --short HEAD 2>/dev/null || echo "unknown")

DEFINES := \
	-DCOMMIT_HASH="\"$(COMMIT_HASH)\"" \
	-DOS_INFO="\"$(OS_INFO)\"" \
	-DBUILD_MODE="\"$(MODE)\"" \
	-DHTTP_BACKEND="\"$(HTTP_BACKEND)\"" \
	-DENVCFLAGS="\"$(ENVCFLAGS)\""

WARNINGS := \
	-Wall \
	-Wextra \
	-Wno-unused-parameter \
	-Wno-unused-function \
	-Wno-unused \
	-Wno-format \
	-Wno-unknown-warning-option

CFLAGS2 := \
	-std=c17 \
	-fno-strict-aliasing \
	-fwrapv \
	-fPIC \
	$(WARNINGS) \
	$(DEFINES) \
	-Isrc \
	$(ENVCFLAGS)

ifeq ($(WITH_BGWORKER), yes)
SOURCES += \
	src/bg_worker.c \
	src/http_server.c

CFLAGS2 += -DWITH_BGWORKER

ifeq ($(HTTP_BACKEND), mongoose)
CFLAGS2 += -DMG_ENABLE_LOG=0 -DMG_ENABLE_PACKED_FS=0 -DMG_ENABLE_LINES=0
SOURCES += src/http_server_backend/mongoose.c vendor/mongoose.c
else ifeq ($(HTTP_BACKEND), dumb)
SOURCES += src/http_server_backend/dumb.c
endif
endif

OBJS := $(SOURCES:%.c=src/o/$(MODE)/%.o)

ifeq ($(MODE), rel)
CFLAGS2 += -O2 -DNDEBUG
else ifeq ($(MODE), dbg)
CFLAGS2 += \
	-fno-omit-frame-pointer \
	-g3 \
	-O0
else
$(error Invalid MODE=$(MODE). Available modes are "dbg", "rel")
endif

dirs:
	mkdir -p src/o/$(MODE)/src
	mkdir -p src/o/$(MODE)/src/http_server_backend
	mkdir -p src/o/$(MODE)/src/compatibility
	mkdir -p src/o/$(MODE)/vendor

src/o/$(MODE)/%.o: %.c | dirs
	printf "\t%-42s $@\n" $<
	$(CC) $(CPPFLAGS) $(CFLAGS2) -c $< -o $@
