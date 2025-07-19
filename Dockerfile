# syntax=docker/dockerfile:1.7
FROM dev-env

USER dev
WORKDIR /workspace

# --- Private dependency repos (requires secret 'gh') ---
# Build with:
#   DOCKER_BUILDKIT=1 docker build --secret id=gh,env=GITHUB_TOKEN -t my-image .
# Comment out if deps are public.
RUN --mount=type=secret,id=gh,uid=1000,gid=1000,mode=0400,required \
    set -eux; GHTOKEN="$(cat /run/secrets/gh)"; \
    for repo in a-cmake-library the-macro-library a-memory-library the-lz4-library; do \
      git clone "https://${GHTOKEN}@github.com/knode-ai-open-source/${repo}.git" "$repo"; \
      (cd "$repo" && ./build_install.sh); \
      rm -rf "$repo"; \
    done

# --- Project source ---
COPY --chown=dev:dev . /workspace/code
RUN cd /workspace/code && ./build_install.sh

CMD ["/bin/bash"]
