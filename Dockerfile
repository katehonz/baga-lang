# syntax=docker/dockerfile:1
# Sandak multi-stage build: git URL -> компилиран Baga бинарник в slim образ.
# Параметри (виж docker-compose.yml):
#   BAGA_REPO/BAGA_REF — откъде идва toolchain-ът (компилатор + sandak)
#   APP_REPO/APP_REF   — приложението; APP_DIR — поддиректория в него (monorepo)

FROM debian:bookworm-slim AS toolchain
RUN apt-get update \
 && apt-get install -y --no-install-recommends gcc libc6-dev make git ca-certificates \
 && rm -rf /var/lib/apt/lists/*
ARG BAGA_REPO=https://git.bara-lang.org/baga-lang-ai/baga-lang-ai.git
ARG BAGA_REF=main
RUN git clone --depth 1 --branch "$BAGA_REF" "$BAGA_REPO" /baga \
 && make -C /baga all sandak \
 && cp /baga/baga /baga/sandak /usr/local/bin/

FROM toolchain AS build
ARG APP_REPO
ARG APP_REF=main
ARG APP_DIR=.
RUN test -n "$APP_REPO" || { echo "APP_REPO е задължителен (--build-arg)"; exit 1; }
# sandak изисква basename на пакетната директория == name в sandak.toml.
# При APP_DIR=. това е самото clone-място, затова го кръщаваме с името на
# пакета; при monorepo subdir-ът вече носи правилното име, root-ът е "repo".
# --locked: възпроизводим build; deps се теглят от GitHub/git по sandak.lock.
# Две стъпки: `fetch` първо генерира свеж lock за path-dep (monorepo) builds —
# техните lock-ове съдържат абсолютни пътища и не са преносими — после --locked
# го валидира. Apps с git deps комитват преносим lock и --locked минава директно:
# dep sources са git URL-и, а root се записва константно като "path+." —
# абсолютният път на checkout-а не влиза в lock файла.
RUN git clone --depth 1 --branch "$APP_REF" "$APP_REPO" /tmp/src \
 && mkdir -p /pkg \
 && name=$(sed -n 's/^name *= *"\([^"]*\)".*/\1/p' "/tmp/src/$APP_DIR/sandak.toml" | head -1) \
 && test -n "$name" || { echo "липсва name в $APP_DIR/sandak.toml"; exit 1; } \
 && if [ "$APP_DIR" = "." ]; then mv /tmp/src "/pkg/$name"; pkgdir="/pkg/$name"; \
    else mv /tmp/src /pkg/repo; pkgdir="/pkg/repo/$APP_DIR"; fi \
 && cd "$pkgdir" \
 && sandak fetch && sandak build --locked && mkdir -p /out \
 && rm -f target/*.c \
 && cp target/* /out/app

FROM debian:bookworm-slim
COPY --from=build /out/app /usr/local/bin/app
CMD ["app"]
