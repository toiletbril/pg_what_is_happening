# Docker image for pg_what_is_happening tests.

FROM docker.io/library/alpine:latest

SHELL ["sh", "-eu", "-c"]

RUN apk update
RUN apk add \
    build-base \
    musl-dev \
    linux-headers \
    git \
    openssl-dev \
    libxml2-dev \
    bsd-compat-headers \
    fts-dev \
    pkgconf \
    python3 \
    readline-dev \
    perl \
    bison \
    flex \
    bash \
    musl-locales \
    libgcc \
    gdb \
    curl \
    sudo \
    icu-dev \
    tmux

RUN adduser -D -u 1000 "postgres" && \
    echo "postgres ALL=(ALL) NOPASSWD: ALL" >> "/etc/sudoers"
RUN mkdir -p "/postgres-bin" "/data" && \
    chown postgres:postgres "/data" "/postgres-bin"

ENV PATH="/postgres-bin/bin:$PATH"

WORKDIR "/pg_what_is_happening"
