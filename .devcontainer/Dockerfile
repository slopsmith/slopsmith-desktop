# Dockerfile for reproducible Slopsmith Desktop builds
# Mirrors the GitHub Actions ubuntu-22.04 runner environment
# Uses versions from .build-config.json

FROM mcr.microsoft.com/devcontainers/base:ubuntu-22.04

# Prevent interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Install jq first (needed to parse build config)
RUN apt-get update && apt-get install -y --no-install-recommends \
    jq \
    && rm -rf /var/lib/apt/lists/*

# Copy and validate build configuration
COPY .build-config.json /tmp/.build-config.json
RUN jq empty /tmp/.build-config.json || (echo "Error: .build-config.json is not valid JSON" && exit 1)

# Extract versions from config
RUN NODE_VERSION=$(jq -r '.versions.node' /tmp/.build-config.json) && \
    PYTHON_VERSION=$(jq -r '.versions.python' /tmp/.build-config.json) && \
    DOTNET_VERSION=$(jq -r '.versions.dotnet' /tmp/.build-config.json) && \
    CMAKE_VERSION=$(jq -r '.versions.cmake' /tmp/.build-config.json) && \
    echo "NODE_VERSION=$NODE_VERSION" >> /etc/build.env && \
    echo "PYTHON_VERSION=$PYTHON_VERSION" >> /etc/build.env && \
    echo "DOTNET_VERSION=$DOTNET_VERSION" >> /etc/build.env && \
    echo "CMAKE_VERSION=$CMAKE_VERSION" >> /etc/build.env

# Source the versions
ENV NODE_VERSION=""
ENV PYTHON_VERSION=""
ENV DOTNET_VERSION=""
ENV CMAKE_VERSION=""
RUN set -a && . /etc/build.env && set +a && \
    export NODE_VERSION PYTHON_VERSION DOTNET_VERSION CMAKE_VERSION

# ── System Dependencies ─────────────────────────────────────────────────────
# Package list is defined in .packages/apt.txt for consistency with GitHub Actions
# Copy .packages directory so we can read the package list
COPY .packages /tmp/.packages
RUN apt-get update && \
    PACKAGES=$(grep -v '^[[:space:]]*#' /tmp/.packages/apt.txt | grep -v '^[[:space:]]*$' | tr '\n' ' ') && \
    apt-get install -y --no-install-recommends $PACKAGES && \
    rm -rf /var/lib/apt/lists/*

# ── Node.js ───────────────────────────────────────────────────────────
# Using NodeSource repository with version from build config
RUN . /etc/build.env && \
    curl -fsSL https://deb.nodesource.com/setup_${NODE_VERSION}.x | bash - \
    && apt-get install -y nodejs \
    && rm -rf /var/lib/apt/lists/*

# ── Python ───────────────────────────────────────────────────────────
# Version from build config
# Note: python3.12 is in Ubuntu 22.04 main repos (not deadsnakes)
# python3.12-distutils needs to be installed from deadsnakes if available,
# otherwise python3-distutils provides distutils that works with 3.12
RUN . /etc/build.env && \
    apt-get update && apt-get install -y --no-install-recommends \
    software-properties-common \
    && add-apt-repository ppa:deadsnakes/ppa \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
    python${PYTHON_VERSION} \
    python${PYTHON_VERSION}-dev \
    python${PYTHON_VERSION}-venv \
    python3-distutils \
    python3-pip \
    python3-setuptools \
    && rm -rf /var/lib/apt/lists/* \
    && ln -sf /usr/bin/python${PYTHON_VERSION} /usr/bin/python3

# ── .NET ─────────────────────────────────────────────────────────────
# Version from build config
RUN . /etc/build.env && \
    curl -fsSL https://dot.net/v1/dotnet-install.sh -o /tmp/dotnet-install.sh \
    && chmod +x /tmp/dotnet-install.sh \
    && /tmp/dotnet-install.sh --channel ${DOTNET_VERSION} --install-dir /usr/share/dotnet \
    && ln -sf /usr/share/dotnet/dotnet /usr/local/bin/dotnet \
    && rm /tmp/dotnet-install.sh

ENV DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1
ENV DOTNET_CLI_TELEMETRY_OPTOUT=1

# ── Container User ────────────────────────────────────────────────────────
# The base image creates a 'vscode' user (UID 1000) for development
# Ensure this user owns the workspace
USER vscode

# Set working directory
WORKDIR /workspace

# ── Verification ──────────────────────────────────────────────────────────
# Print versions on container start (helpful for debugging)
RUN . /etc/build.env && \
    echo "=== Build Configuration ===" && \
    echo "Node version: $(node --version)" && \
    echo "npm version: $(npm --version)" && \
    echo "Python version: $(python3 --version)" && \
    echo ".NET version: $(dotnet --version)" && \
    echo "Electron version (from package.json): $(cat /workspace/package.json | jq -r '.devDependencies.electron // "not installed yet"')"
