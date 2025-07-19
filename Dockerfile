# syntax=docker/dockerfile:1
ARG GITHUB_TOKEN
# from docker-setup repo
FROM dev-env

# Make sure build‑time ARG is in scope
ARG GITHUB_TOKEN

# Switch to non‑root “dev” user (already created in dev‑env)
USER dev
WORKDIR /workspace

# a-cmake-library
RUN git clone \
      https://${GITHUB_TOKEN}@github.com/knode-ai-open-source/a-cmake-library.git \
      /workspace/a-cmake-library && \
      cd /workspace/a-cmake-library && ./build_install.sh && \
      rm -rf /workspace/a-cmake-library

# the-macro-library
RUN git clone \
      https://${GITHUB_TOKEN}@github.com/knode-ai-open-source/the-macro-library.git \
      /workspace/the-macro-library && \
      cd /workspace/the-macro-library && ./build_install.sh && \
      rm -rf /workspace/the-macro-library

# a-memory-library
RUN git clone \
      https://${GITHUB_TOKEN}@github.com/knode-ai-open-source/a-memory-library.git \
      /workspace/a-memory-library && \
      cd /workspace/a-memory-library && ./build_install.sh && \
      rm -rf /workspace/a-memory-library

# the-lz4-library
RUN git clone \
      https://${GITHUB_TOKEN}@github.com/knode-ai-open-source/the-lz4-library.git \
      /workspace/the-lz4-library && \
      cd /workspace/the-lz4-library && ./build_install.sh && \
      rm -rf /workspace/the-lz4-library


# Build library
COPY --chown=dev:dev . /workspace/code
RUN cd /workspace/code && ./build_install.sh

# drop into a shell by default
CMD ["/bin/bash"]
