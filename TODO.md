# TODO

Things worth doing, not yet started.

## Online firmware updates

A head that can update itself, rather than one that waits for somebody with
a checkout of this repository and a working PlatformIO install.

Today's over-the-air update still starts on a computer — `pio run -t upload`
or `tools/ota.py` pushes a binary the operator already has. Nothing has been
designed yet; see [Networking](docs/NETWORK.md) for what exists now.
