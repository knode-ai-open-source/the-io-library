### BUILDING

#### 1 · Build the base image once

```bash
git clone https://github.com/knode-ai-open-source/dev-env.git
cd dev-env
docker build -t dev-env .
```

---

#### 2 · Build your project image

```bash
docker build -t the-io-library:dev .
```

---

#### 3 · Project layout

```
Dockerfile        # extends dev-env
build_install.sh  # builds & installs project to /usr/local
src/ …
```

Recommended `build_install.sh`:

```bash
#!/usr/bin/env bash
set -euxo pipefail
rm -rf build && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j"$(nproc)"
sudo make install
```

---

#### 4 · Minimal **Dockerfile** for public dependencies

```dockerfile
# syntax=docker/dockerfile:1.7
FROM dev-env

USER dev
WORKDIR /workspace

RUN set -eux; \
    for repo in a-cmake-library the-macro-library a-memory-library; do \
        git clone --depth 1 "https://github.com/knode-ai-open-source/${repo}.git" "$repo"; \
        (cd "$repo" && ./build_install.sh); \
        rm -rf "$repo"; \
    done

COPY --chown=dev:dev . /workspace/project
RUN cd /workspace/project && ./build_install.sh

CMD ["/bin/bash"]
```

---

#### 5 · Smoke‑test the image (optional)

```bash
docker run --rm -it the-io-library:dev bash -lc \
  'cd /workspace/project/build && ctest --output-on-failure'
```

---

## Advanced usage — private repositories

### a) Install GitHub CLI (`gh`)

*macOS (Homebrew)*

```bash
brew install gh
gh auth login          # HTTPS, follow prompts
```

*Debian/Ubuntu*

```bash
sudo apt update && sudo apt install -y gh
gh auth login
```

Verify:

```bash
gh auth token | wc -c   # >0 means a token is present
```

### b) Extend the Dockerfile

Uncomment the block below and add your private repo names:

```dockerfile
# --- OPTIONAL: private deps (requires BuildKit secret) ---
# RUN --mount=type=secret,id=gh,uid=1000,gid=1000,mode=0400,required \
#     set -eux; GHTOKEN="$(cat /run/secrets/gh)"; \
#     for repo in some-private-repo another-private-repo; do \
#         git clone "https://${GHTOKEN}@github.com/knode-ai/${repo}.git" "$repo"; \
#         (cd "$repo" && ./build_install.sh); \
#         rm -rf "$repo"; \
#     done
```

### c) Build with a token

```bash
GITHUB_TOKEN=$(gh auth token) docker build \
  --secret id=gh,env=GITHUB_TOKEN \
  -t the-io-library:dev .
```

### d) Common token‑related issues

| Message / Symptom                         | Fix                                                                            |
| ----------------------------------------- | ------------------------------------------------------------------------------ |
| `cat: /run/secrets/gh: Permission denied` | Add `uid=1000,gid=1000,mode=0400` to secret mount.                             |
| `Missing GitHub token secret`             | Pass `--secret id=gh,env=GITHUB_TOKEN` (or `src=`).                            |
| `fatal: could not read Username…`         | Token not passed / empty; verify token & secret mount.                         |
| `403` from GitHub                         | Ensure token has **Contents → Read** (fine‑grained) or `repo` scope (classic). |
