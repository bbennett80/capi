.PHONY: all test sanitize poll bench clean
BUILD ?= build

all:
	cmake -S . -B $(BUILD) -DCMAKE_BUILD_TYPE=Release
	cmake --build $(BUILD) -j

test: all
	ctest --test-dir $(BUILD) --output-on-failure

sanitize:
	cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug -DCAPI_SANITIZE=ON
	cmake --build build-sanitize -j
	ctest --test-dir build-sanitize --output-on-failure

poll:
	cmake -S . -B build-poll -DCMAKE_BUILD_TYPE=Debug -DCAPI_FORCE_POLL=ON
	cmake --build build-poll -j
	ctest --test-dir build-poll --output-on-failure

bench: all
	@echo "Start build/capi_hello in another terminal, then run build/capi_bench 127.0.0.1 8080 64 100000"

clean:
	cmake -E rm -rf build build-sanitize build-poll
