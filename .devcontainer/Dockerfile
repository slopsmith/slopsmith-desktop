# Dockerfile for reproducible Slopsmith Desktop builds.
# Mirrors the GitHub Actions ubuntu-22.04 runner environment. Versions
# are read from .build-config.json so CI and this container can't drift.
#
# Reproducibility caveat: the base tag `ubuntu-22.04` tracks the latest
# security-patched Microsoft devcontainer image. True byte-for-byte
# reproducibility would pin to an image digest; in practice a rolling
# `ubuntu-22.04` matches what the GitHub Actions runner ships.
FROM mcr.microsoft.com/devcontainers/base:ubuntu-22.04

ENV DEBIAN_FRONTEND=noninteractive

# ── Build config ────────────────────────────────────────────────────────────
# Python is preinstalled on the base image; use it to read version pins
# from .build-config.json (avoids adding jq to the image).
COPY .build-config.json /tmp/.build-config.json
COPY scripts/parse-build-config.py /tmp/parse-build-config.py

RUN NODE_VERSION=$(python3 /tmp/parse-build-config.py /tmp/.build-config.json .versions.node) && \
    PYTHON_VERSION=$(python3 /tmp/parse-build-config.py /tmp/.build-config.json .versions.python) && \
    DOTNET_VERSION=$(python3 /tmp/parse-build-config.py /tmp/.build-config.json .versions.dotnet) && \
    PYTHON_MAJOR_MINOR="${PYTHON_VERSION%.*}" && \
    printf 'NODE_VERSION=%s\nPYTHON_VERSION=%s\nPYTHON_MAJOR_MINOR=%s\nDOTNET_VERSION=%s\n' \
        "$NODE_VERSION" "$PYTHON_VERSION" "$PYTHON_MAJOR_MINOR" "$DOTNET_VERSION" > /etc/build.env

# ── System dependencies ─────────────────────────────────────────────────────
# Single source of truth: .packages/apt.txt (also consumed by CI).
COPY .packages /tmp/.packages
RUN apt-get update && \
    PACKAGES=$(grep -v '^[[:space:]]*#' /tmp/.packages/apt.txt | grep -v '^[[:space:]]*$' | tr '\n' ' ') && \
    apt-get install -y --no-install-recommends $PACKAGES && \
    rm -rf /var/lib/apt/lists/*

# ── Node.js ─────────────────────────────────────────────────────────────────
RUN . /etc/build.env && \
    curl -fsSL https://deb.nodesource.com/setup_${NODE_VERSION}.x | bash - && \
    apt-get install -y nodejs && \
    rm -rf /var/lib/apt/lists/*

# ── .NET ────────────────────────────────────────────────────────────────────
RUN . /etc/build.env && \
    curl -fsSL https://dot.net/v1/dotnet-install.sh -o /tmp/dotnet-install.sh && \
    chmod +x /tmp/dotnet-install.sh && \
    /tmp/dotnet-install.sh --channel ${DOTNET_VERSION} --install-dir /usr/share/dotnet && \
    ln -sf /usr/share/dotnet/dotnet /usr/local/bin/dotnet && \
    rm /tmp/dotnet-install.sh

ENV DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1
ENV DOTNET_CLI_TELEMETRY_OPTOUT=1

# ── Workspace ───────────────────────────────────────────────────────────────
USER vscode
WORKDIR /workspace

# Runtime verification. Workspace isn't mounted at image-build time —
# package.json only exists when the container is RUN against a mounted
# project, so the Electron version check runs at container start rather
# than build. Print what we CAN verify here.
RUN . /etc/build.env && \
    echo "=== Slopsmith Desktop build environment ===" && \
    echo "Node: $(node --version)" && \
    echo "npm:  $(npm --version)" && \
    echo "Python: $(python3 --version)" && \
    echo ".NET: $(dotnet --version)"
