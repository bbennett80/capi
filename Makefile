.PHONY: all test sanitize poll extras workers thread-sanitize bench clean
BUILD ?= build
CMAKE_ARGS ?=

all:
	cmake -S . -B $(BUILD) -DCMAKE_BUILD_TYPE=Release $(CMAKE_ARGS)
	cmake --build $(BUILD) -j

test: all
	ctest --test-dir $(BUILD) --output-on-failure

sanitize:
	cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug -DCAPI_SANITIZE=ON $(CMAKE_ARGS)
	cmake --build build-sanitize -j
	ctest --test-dir build-sanitize --output-on-failure

poll:
	cmake -S . -B build-poll -DCMAKE_BUILD_TYPE=Debug -DCAPI_FORCE_POLL=ON $(CMAKE_ARGS)
	cmake --build build-poll -j
	ctest --test-dir build-poll --output-on-failure

extras:
	cmake -S . -B build-extras -DCMAKE_BUILD_TYPE=Release -DCAPI_WITH_SQLITE=ON -DCAPI_WITH_TEMPLATES=ON $(CMAKE_ARGS)
	cmake --build build-extras -j
	ctest --test-dir build-extras --output-on-failure

workers:
	cmake -S . -B build-workers -DCMAKE_BUILD_TYPE=Release -DCAPI_WITH_WORKERS=ON $(CMAKE_ARGS)
	cmake --build build-workers -j
	ctest --test-dir build-workers --output-on-failure

thread-sanitize:
	cmake -S . -B build-workers-tsan -DCMAKE_BUILD_TYPE=Debug -DCAPI_WITH_WORKERS=ON -DCAPI_THREAD_SANITIZE=ON $(CMAKE_ARGS)
	cmake --build build-workers-tsan -j
	ctest --test-dir build-workers-tsan --output-on-failure

bench: all
	@echo "Start build/capi_hello in another terminal, then run build/capi_bench 127.0.0.1 8080 64 100000"

clean:
	cmake -E rm -rf build build-sanitize build-poll build-extras build-workers build-workers-tsan
