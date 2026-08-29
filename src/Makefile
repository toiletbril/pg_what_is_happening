SOURCES = \
	src/pg_what_is_happening.c \
	src/shared_memory.c \
	src/plan_tree_walker.c \
	src/signal_handler.c \
	src/compatibility.c \
	src/gucs.c \
	src/metrics.c

ENVCFLAGS := $(CFLAGS)
PG_BUILD_ID := $(shell "$(PG_CONFIG)" --version 2>/dev/null | tr -cs 'A-Za-z0-9.' '_')
BUILD_KEY := $(MODE)-bg$(WITH_BGWORKER)-$(HTTP_BACKEND)-$(PG_BUILD_ID)
BUILD_CONFIG := src/o/$(BUILD_KEY)/.build-config

ifneq ($(filter $(WITH_BGWORKER),yes no),$(WITH_BGWORKER))
$(error Invalid WITH_BGWORKER=$(WITH_BGWORKER). Available values are "yes", "no")
endif

ifneq ($(filter $(HTTP_BACKEND),mongoose dumb),$(HTTP_BACKEND))
$(error Invalid HTTP_BACKEND=$(HTTP_BACKEND). Available backends are "mongoose", "dumb")
endif

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
	-Wshadow \
	-Wno-unused-parameter \
	-Wno-unused-function \
	-Wno-unused \
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

OBJS := $(SOURCES:%.c=src/o/$(BUILD_KEY)/%.o)
DEPS := $(OBJS:.o=.d)

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
	mkdir -p src/o/$(BUILD_KEY)/src
	mkdir -p src/o/$(BUILD_KEY)/src/http_server_backend
	mkdir -p src/o/$(BUILD_KEY)/src/compatibility
	mkdir -p src/o/$(BUILD_KEY)/vendor

$(BUILD_CONFIG): FORCE | dirs
	printf '%s\n' 'PG_CONFIG=$(PG_CONFIG)' 'CC=$(CC)' 'CPPFLAGS=$(CPPFLAGS)' 'CFLAGS2=$(CFLAGS2)' 'LDFLAGS=$(LDFLAGS)' > $@.tmp
	if ! cmp -s $@.tmp $@; then mv $@.tmp $@; else rm -f $@.tmp; fi

src/o/$(BUILD_KEY)/%.o: %.c $(BUILD_CONFIG) | dirs
	printf "\t%-42s $@\n" $<
	$(CC) $(CPPFLAGS) $(CFLAGS2) -MMD -MP -c $< -o $@

.PHONY: FORCE
FORCE:

-include $(wildcard $(DEPS))
