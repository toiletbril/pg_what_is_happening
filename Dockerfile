#
# Docker image for pg_what_is_happening tests.
#

FROM docker.io/library/alpine:3.22@sha256:14358309a308569c32bdc37e2e0e9694be33a9d99e68afb0f5ff33cc1f695dce

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
    diffutils \
    tmux \
    util-linux \
    ncurses

RUN adduser -D -u 1000 "postgres" && \
    echo "postgres ALL=(ALL) NOPASSWD: ALL" >> "/etc/sudoers"

RUN mkdir -p /postgres /postgres-build /postgres-bin /data /pg_what_is_happening

# Steal host's tmux.conf for the container.
COPY .tmux.conf /etc/tmux.conf

USER postgres
WORKDIR "/pg_what_is_happening"
