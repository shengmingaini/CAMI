# CAMI Benchmark — TCP Echo Verification

> [PROTOTYPE] Tech stack verification for C++17 + boost::asio

## Quick Start

### Prerequisites

- C++17 compiler (g++ ≥ 9, clang++ ≥ 10, or MSVC ≥ 19.20)
- CMake ≥ 3.20
- vcpkg with `VCPKG_ROOT` set

### Build

```bash
cd /path/to/CAMI
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release \
         -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build . --target tcp_echo_server tcp_echo_benchmark -j$(nproc)
```

### Run

**Terminal 1 — Start server:**
```bash
./build/bin/tcp_echo_server 9999 $(nproc)
```

**Terminal 2 — Run benchmark:**
```bash
# Usage: tcp_echo_benchmark [host] [port] [connections] [msg_size] [total_msgs] [pipeline]
./build/bin/tcp_echo_benchmark 127.0.0.1 9999 32 64 1000000 8
```

### One-Click Runner

```bash
bash scripts/run_benchmark.sh
```

## Files

| File | Description |
|------|-------------|
| `tcp_echo_server.cpp` | Async TCP echo server (boost::asio, multi-threaded) |
| `tcp_echo_benchmark.cpp` | Multi-connection benchmark client with pipelining |
| `CMakeLists.txt` | Build configuration |

## Architecture

- **Server**: async_accept → Session(async_read → async_write loop), io_context multi-threaded
- **Client**: N connections × pipeline depth M, async I/O, atomic counters
- **TCP_NODELAY**: Disabled Nagle's algorithm for low-latency echo

## Target

- QPS ≥ 100,000 (echo round-trips per second)
- Average latency < 100μs per round-trip
