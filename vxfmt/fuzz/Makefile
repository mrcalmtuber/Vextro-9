# Fuzzing vx_validate().
#
# Two binaries, and the difference matters:
#
#   harness       instrumented by afl-clang-fast, with ASan compiled in.
#                 This is what afl-fuzz drives. Coverage feedback comes
#                 from the instrumentation, so it MUST be built with
#                 afl-clang-fast and not with a plain compiler -- an
#                 uninstrumented target still "runs" under afl-fuzz and
#                 still finds nothing, which is the failure mode this
#                 file exists to make hard.
#
#   harness-repl  ASan only, no instrumentation. For replaying a crash
#                 found by the fuzzer and reading the report without
#                 AFL's forkserver in the way, and for confirming under
#                 a second toolchain that a crash is real.
#
# Nothing here needs anything beyond afl++ and a C compiler.

AFL_CC   ?= afl-clang-fast
REPL_CC  ?= clang

CFLAGS_COMMON := -g -O1 -Wall -Wextra -std=gnu11 -I..

# -fno-omit-frame-pointer keeps ASan's stack traces readable at -O1.
ASAN_FLAGS := -fsanitize=address -fno-omit-frame-pointer

.PHONY: all clean fuzz check-seeds

all: harness harness-repl

# AFL_USE_ASAN is read by the afl-* compiler wrappers at *compile* time,
# so it is set on the recipe rather than expected in the environment.
harness: harness.c ../vx_format.h
	AFL_USE_ASAN=1 $(AFL_CC) $(CFLAGS_COMMON) -o $@ $<

harness-repl: harness.c ../vx_format.h
	$(REPL_CC) $(CFLAGS_COMMON) $(ASAN_FLAGS) -o $@ $<

# Every seed must actually validate. A corpus that all fails at byte 0
# gives the fuzzer nothing to mutate towards.
check-seeds: harness-repl
	@bad=0; n=0; \
	for f in in/*; do \
	    [ -f "$$f" ] || continue; \
	    n=$$((n+1)); \
	    if ./harness-repl "$$f" >/dev/null 2>&1; then \
	        echo "  ok      $$f"; \
	    else \
	        echo "  REJECTS $$f"; bad=$$((bad+1)); \
	    fi; \
	done; \
	echo "  $$n seed(s), $$bad rejected"; \
	[ "$$bad" -eq 0 ]

clean:
	rm -f harness harness-repl
