FROM mcr.microsoft.com/devcontainers/base:ubuntu-24.04

# Avoid interactive prompt hangs
ENV DEBIAN_FRONTEND=noninteractive

# 1. Install C++ compilers, SCons, and core tools
RUN apt-get update && apt-get install -y \
    build-essential \
    scons \
    pkg-config \
    clang \
    clang-format \
    curl \
    git \
    && rm -rf /var/lib/apt/lists/*

# 2. Install Node.js manually (Required for Claude Code)
RUN curl -fsSL https://deb.nodesource.com/setup_22.x | bash - \
    && apt-get install -y nodejs

# 3. Pre-install Claude Code globally via npm
RUN npm install -g @anthropic-ai/claude-code