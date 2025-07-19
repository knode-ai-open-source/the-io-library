# BUILDING

**Quick‑start – one inline secret build flow**

---

## 1 · Build the base image once

```bash
git clone https://github.com/Knode-ai-open-source/dev-env.git
cd dev-env
docker build -t dev-env .
```
---

## 2 · Build your extension image (from your project dir that has the Dockerfile)
GITHUB_TOKEN=$(gh auth token) docker build \
--secret id=gh,env=GITHUB_TOKEN \
-t the-io-library:dev .

---

## 3 · Project layout

```
Dockerfile        # extends dev-env
build_install.sh  # builds & installs project to /usr/local
src/ …
```

Recommended `build_install.sh` template:

```bash
#!/usr/bin/env bash
set -euxo pipefail
rm -rf build && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j"$(nproc)"
sudo make install
```

---

## 4 · Minimal extension **Dockerfile**

Token‑protected dependencies are cloned as user **dev** (uid 1000) with a BuildKit secret mount that never ends up in a layer.

```dockerfile
# syntax=docker/dockerfile:1.7
FROM dev-env

USER dev
WORKDIR /workspace

RUN --mount=type=secret,id=gh,uid=1000,gid=1000,mode=0400,required \
    set -eux; GHTOKEN="$(cat /run/secrets/gh)"; \
    for repo in a-cmake-library the-macro-library a-memory-library; do \
      git clone "https://${GHTOKEN}@github.com/knode-ai-open-source/${repo}.git" "$repo"; \
      (cd "$repo" && ./build_install.sh); \
      rm -rf "$repo"; \
    done

COPY --chown=dev:dev . /workspace/project
RUN cd /workspace/project && ./build_install.sh

CMD ["/bin/bash"]
```

> If all dependencies are public, you can delete the `RUN` block.

---

## 5 · Build with **one inline command**

```bash
GITHUB_TOKEN=$(gh auth token) docker build \
  --secret id=gh,env=GITHUB_TOKEN \
  -t the-io-library:dev .
```

BuildKit is enabled by default on Docker Desktop; you can drop `DOCKER_BUILDKIT=1`.

---

## 6 · Smoke‑test the image (optional)

```bash
docker run --rm -it the-io-library:dev bash -lc \
  'cd /workspace/project/build && ctest --output-on-failure'
```

---

## 7 · Troubleshooting

| Symptom / Message                                         | Likely Cause                                       | Fix (quick)                                                                                  |
| --------------------------------------------------------- | -------------------------------------------------- | -------------------------------------------------------------------------------------------- |
| `cat: /run/secrets/gh: Permission denied`                 | Secret owned by root; running as `dev`             | Add `uid=1000,gid=1000,mode=0400` to secret mount (or run step as root before `USER dev`).   |
| `Missing GitHub token secret` (from `required`)           | Secret not passed                                  | Add `--secret id=gh,src=.gh-token` (or working inline method); ensure id matches Dockerfile. |
| `fatal: could not read Username for 'https://github.com'` | Empty / missing token (anonymous clone of private) | Pass secret correctly; verify token length & scopes.                                         |
| `403` from GitHub                                         | Token lacks repo access                            | Fine‑grained: grant **Contents: Read**; classic: ensure `repo` scope; re-run build.          |
| `BYTES:0` in minimal secret test                          | Secret file empty                                  | Regenerate token file (`gh auth token > .gh-token`) or correct inline command.               |
| Works with `src=` but not `env=`                          | Docker build path ignores `env=` form              | Use `--secret id=gh,src=.gh-token` (recommended) or `buildx build` if you need `env=`.       |
| `Permission denied` after adding uid/gid                  | Wrong uid                                          | Confirm `id -u dev` (usually 1000) and match `uid=`.                                         |
| Token length unexpectedly short (0)                       | Not logged in / expired session                    | `gh auth login` again; then `gh auth token | wc -c` (expect >0).                               |

### Install & Authenticate `gh`

**macOS (Homebrew):**
```bash
brew install gh
````

**Debian/Ubuntu:**

```bash
sudo apt update && sudo apt install -y gh || {
  type -p curl >/dev/null || sudo apt install -y curl;
  echo 'If gh not in repo, see https://cli.github.com/';
}
```

**Login (HTTPS):**

```bash
gh auth login   # choose GitHub.com, HTTPS, follow prompts; select scopes
```

**Verify token present:**

```bash
gh auth token | wc -c   # expect >0 (e.g. 40 or 41)
```

### Minimal Secret Test

```bash
echo "$(gh auth token)" > .gh-token
cat > SecretTest <<'EOF'
# syntax=docker/dockerfile:1.7
FROM alpine
RUN --mount=type=secret,id=gh,required sh -c 'echo -n BYTES:; wc -c /run/secrets/gh'
EOF
docker build --secret id=gh,src=.gh-token -f SecretTest .
rm -f SecretTest .gh-token
# Expect BYTES:40 (or similar non-zero)
```

Apache‑2.0 · © 2019‑2025 Andy Curtis · contact: [contactandyc@gmail.com](mailto:contactandyc@gmail.com)
