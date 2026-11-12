# LockSpy - GNU Make build.
#   make          build build/libLockSpy.so and every test program
#   make test     run the test suite under LockSpy (tests/run_tests.sh)
#   make demo     AB-BA demo + rendered lock graph (needs Graphviz for PNG/SVG)
#   make bench    overhead + tool comparison (bench/run_bench.sh -> results/RESULTS.md)
#   make memcheck run the test programs with LockSpy under Valgrind memcheck
#
# LockSpy needs Linux. On any other OS (macOS) every target except clean is run inside the
# lockspy-dev Ubuntu container by bin/in-docker, so "make test" works the same everywhere.
ifneq ($(shell uname -s),Linux)
.PHONY: all test bench demo memcheck clean
all test bench demo memcheck:
	@./bin/in-docker make $@
clean:
	rm -rf build results/tests results/demo
else

CXX      ?= g++
CXXFLAGS := -std=c++17 -O2 -fPIC -Wall -Wextra -fno-omit-frame-pointer
TESTFLAGS:= -std=c++17 -g -O0 -rdynamic -pthread -no-pie -Wall -Wextra
LIB      := build/libLockSpy.so
SRCS     := src/config.cpp src/recorder.cpp src/lock_graph.cpp src/waitfor.cpp src/profiler.cpp \
            src/symbolize.cpp src/report.cpp src/interpose.cpp
OBJS     := $(SRCS:src/%.cpp=build/obj/%.o)
TEST_SRCS:= $(wildcard tests/*/*.cpp)
TEST_BINS:= $(TEST_SRCS:tests/%.cpp=build/tests/%)

.PHONY: all test bench demo memcheck clean
all: $(LIB) $(TEST_BINS)

build/obj/%.o: src/%.cpp src/*.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(LIB): $(OBJS)
	$(CXX) -shared -o $@ $(OBJS) -ldl -pthread

build/tests/%: tests/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(TESTFLAGS) $< -o $@

test: all
	./tests/run_tests.sh

bench: all
	./bench/run_bench.sh

demo: all
	@mkdir -p results/demo
	-./bin/lockspy run --format text,json,dot,html --outdir results/demo -- ./build/tests/buggy/ab_ba
	@if command -v dot >/dev/null; then \
	  dot -Tpng results/demo/lockspy_graph.dot -o results/demo/lockspy_graph.png; \
	  dot -Tsvg results/demo/lockspy_graph.dot -o results/demo/lockspy_graph.svg; \
	  echo "rendered results/demo/lockspy_graph.png and .svg"; \
	else echo "Graphviz not installed: DOT file only"; fi

memcheck: all
	./tests/memcheck.sh

clean:
	rm -rf build results/tests results/demo

endif
