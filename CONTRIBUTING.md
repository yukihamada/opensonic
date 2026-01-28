# Contributing to Soluna

Thank you for your interest in contributing to Soluna! This document provides guidelines and instructions for contributing.

## Code of Conduct

Please be respectful and constructive in all interactions. We welcome contributors of all experience levels.

## Getting Started

### Prerequisites

- **Linux**: GCC 9+ or Clang 10+, CMake 3.16+, ALSA development libraries
- **macOS**: Xcode Command Line Tools, CMake 3.16+
- **Windows**: Visual Studio 2019+ or MinGW-w64, CMake 3.16+

### Building from Source

```bash
# Clone the repository
git clone https://github.com/your-org/soluna.git
cd soluna

# Create build directory
cmake -B build -DSOLUNA_BUILD_TESTS=ON
cmake --build build --parallel

# Run tests
ctest --test-dir build --output-on-failure
```

### Optional Features

Enable additional features during configuration:

```bash
cmake -B build \
  -DSOLUNA_BUILD_TESTS=ON \
  -DSOLUNA_ENABLE_AES67=ON \
  -DSOLUNA_ENABLE_DTLS=ON \
  -DSOLUNA_ENABLE_AAC=ON \
  -DSOLUNA_ENABLE_FLAC=ON
```

## Development Workflow

### Branch Naming

- `feature/description` — New features
- `fix/description` — Bug fixes
- `docs/description` — Documentation updates
- `refactor/description` — Code refactoring

### Commit Messages

Write clear, concise commit messages:

```
Add jitter buffer statistics collection

- Track packet arrival times
- Calculate jitter metrics
- Export to Prometheus
```

### Pull Request Process

1. Fork the repository and create your branch from `main`
2. Make your changes with appropriate tests
3. Ensure all tests pass: `ctest --test-dir build`
4. Update documentation if needed
5. Submit a pull request with a clear description

### Code Style

- Use 4 spaces for indentation (no tabs)
- Keep lines under 100 characters
- Use `snake_case` for functions and variables
- Use `PascalCase` for classes and structs
- Include header guards or `#pragma once`
- Document public APIs with Doxygen-style comments

Example:

```cpp
/**
 * @brief Calculate the optimal jitter buffer depth
 * @param network_jitter Current network jitter in nanoseconds
 * @param target_latency Target latency in milliseconds
 * @return Recommended buffer depth in samples
 */
size_t calculate_buffer_depth(int64_t network_jitter, uint32_t target_latency);
```

### Testing

- Add unit tests for new functionality in `tests/unit/`
- Add integration tests for cross-component features in `tests/integration/`
- Tests should be self-contained and not require external resources
- Use descriptive test names: `TEST(JitterBuffer, HandlesPacketReordering)`

## Project Structure

```
soluna/
├── apps/           # Executable applications
│   ├── cli/        # Command-line interface (solctl)
│   ├── daemon/     # Main daemon (solunad)
│   └── esp32/      # ESP32 firmware
├── cmake/          # CMake modules and scripts
├── docs/           # Documentation
├── examples/       # Example code and plugins
├── include/soluna/ # Public headers
├── src/            # Implementation
│   ├── audit/      # Audit logging
│   ├── codec/      # Audio codecs
│   ├── config/     # Configuration
│   ├── control/    # Control protocol
│   ├── core/       # Core utilities
│   ├── metrics/    # Prometheus metrics
│   ├── pal/        # Platform abstraction
│   ├── pipeline/   # Audio pipeline
│   ├── security/   # Authentication/ACL
│   ├── sync/       # Clock synchronization
│   ├── transport/  # Network transport
│   └── wifi/       # WiFi optimizations
├── tests/          # Test suites
│   ├── benchmark/  # Performance benchmarks
│   ├── integration/# Integration tests
│   ├── stress/     # Stress tests
│   └── unit/       # Unit tests
└── web/            # Embedded web UI
```

## Reporting Issues

When reporting issues, please include:

1. **Description**: Clear description of the problem
2. **Steps to Reproduce**: Minimal steps to reproduce the issue
3. **Expected Behavior**: What you expected to happen
4. **Actual Behavior**: What actually happened
5. **Environment**: OS, compiler version, CMake version
6. **Logs**: Relevant log output or error messages

## Feature Requests

We welcome feature requests! Please describe:

1. **Use Case**: What problem does this solve?
2. **Proposed Solution**: How should it work?
3. **Alternatives**: Other approaches you've considered
4. **Priority**: How important is this for your use case?

## License

By contributing to Soluna, you agree that your contributions will be licensed under the MIT License.

## Questions?

Feel free to open an issue for questions or join our community discussions.
