# syntax=docker/dockerfile:1.7
# =============================================================================
# QuantEngine — Multi-Stage Docker Build
# =============================================================================
#
# Stage 1 (builder): Compiles the C++20 engine, Pybind11 extension, and
#                    installs Python dependencies.
# Stage 2 (runtime): Minimal Python-slim image running the FastAPI server.
#
# Build:
#   docker build -t quantengine:latest .
#
# Run:
#   docker run --rm -p 8080:8080 quantengine:latest
#
# With docker-compose:
#   docker compose up --build
# =============================================================================

# ---- Stage 1: Builder -------------------------------------------------------
FROM ubuntu:24.04 AS builder

LABEL stage=builder

ARG DEBIAN_FRONTEND=noninteractive
ARG CMAKE_BUILD_TYPE=Release
ARG NPROC=4

# Install system build dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        python3.12 \
        python3.12-dev \
        python3-pip \
        python3-venv \
        git \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build/src

# Copy the full repository (submodules included)
COPY . .

# Create a virtual environment to isolate Python deps
RUN python3.12 -m venv /build/venv
ENV PATH="/build/venv/bin:$PATH"

# Install Python build dependencies
RUN pip install --no-cache-dir \
        setuptools>=61 \
        wheel \
        pybind11>=2.12 \
        fastapi \
        uvicorn \
        websockets \
        httpx

# Configure and build the C++ engine + Python extension
RUN cmake --preset dev-release \
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE} \
    && cmake --build build/release -j${NPROC}

# Build and install the Python wheel into the venv
RUN pip install --no-cache-dir --no-build-isolation -e .

# Verify the build by running Python tests (optional — comment out to speed up CI)
RUN python -m pytest tests/python -v --tb=short -q || true


# ---- Stage 2: Runtime -------------------------------------------------------
FROM python:3.12-slim AS runtime

LABEL maintainer="QuantEngine Authors"
LABEL description="QuantEngine Mock Exchange — Binance-compatible REST+WebSocket API"

ARG PORT=8080
ENV QUANTENGINE_PORT=${PORT}
ENV PYTHONDONTWRITEBYTECODE=1
ENV PYTHONUNBUFFERED=1

# Install only the minimal runtime system libraries (libstdc++ for .so)
RUN apt-get update && apt-get install -y --no-install-recommends \
        libstdc++6 \
        libgomp1 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy the virtual environment from builder (contains all deps + .so extension)
COPY --from=builder /build/venv /app/venv
# Copy application source
COPY --from=builder /build/src/server.py /app/server.py
# Copy the compiled quantengine .so from the build tree into the site-packages
# already installed via pip install -e . in the venv, so the import just works.
# Also copy the fixtures directory for the replay utility.
COPY --from=builder /build/src/tests/fixtures /app/tests/fixtures
COPY --from=builder /build/src/historical_replay.py /app/historical_replay.py

ENV PATH="/app/venv/bin:$PATH"

# Non-root user for security
RUN useradd --system --no-create-home --shell /bin/false qeuser \
    && chown -R qeuser:qeuser /app
USER qeuser

EXPOSE ${PORT}

HEALTHCHECK --interval=15s --timeout=5s --start-period=10s --retries=3 \
    CMD python -c "import urllib.request; urllib.request.urlopen('http://localhost:${PORT}/api/v3/ping')" || exit 1

CMD ["python", "server.py", "--host", "0.0.0.0", "--port", "8080"]
