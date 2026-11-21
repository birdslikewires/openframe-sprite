# i386 Debian Trixie build environment for sprite and its Mesa-free display stack.
# Build with: docker build --platform linux/386 -t sprite-build build/
# (CI builds the same image with 'docker run' and prepare.sh: see .github/workflows/package.yml)
FROM i386/debian:trixie
COPY prepare.sh stack.sh /build/
RUN sh /build/prepare.sh
WORKDIR /src
