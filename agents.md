# Build and Run (0.17.3)

This repo is pinned to the 0.17.3 release with small header fixes for modern toolchains.

## Build (tiles)

```sh
cd /media/master/Old\ C/dev/projects/crawl/world-crawl

git submodule update --init --recursive

# Ubuntu/Debian deps for tiles build
sudo apt install -y \
  build-essential libncursesw5-dev bison flex liblua5.4-dev \
  libsqlite3-dev libz-dev pkg-config python3-yaml binutils-gold python-is-python3 \
  libsdl2-image-dev libsdl2-mixer-dev libsdl2-dev libfreetype6-dev libpng-dev \
  fonts-dejavu-core advancecomp pngcrush libglu1-mesa-dev

cd crawl-ref/source
make -j4 TILES=y
```

## Run

```sh
cd /media/master/Old\ C/dev/projects/crawl/world-crawl/crawl-ref/source
./crawl
```

## Notes

- The build emits warnings with newer compilers; the tiles build should still succeed.
- If you want a console-only build, omit `TILES=y`.
