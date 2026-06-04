# Releasing

Binaries are built and published by CI — never committed. To cut a release:

```bash
git tag v0.1.0        # bump the version
git push origin v0.1.0
```

Pushing a `v*` tag runs [`.github/workflows/release.yml`](.github/workflows/release.yml),
which:

1. Builds the **ESP32-S3** bridge (ESP-IDF v6.0.1) and merges it into a single
   `ganymede-bridge-esp32s3.bin` (flashable at `0x0`).
2. Builds the **nRF52840** emulator (nRF Connect SDK v2.7.0) → `ganymede-emulator-nrf52840.uf2`.
   The git-ignored `clone_addr.h` is absent in CI, so the binary **generates a unique
   per-device Cypress-OUI address** at first start and pairs like any new remote
   (plug-and-play; no per-unit configuration).
3. Fetches the one-time nRF **bootloader-update** UF2
   (`update-nice_nano_bootloader-0.9.2_nosd.uf2`).
4. Publishes a **GitHub Release** with all three files + `SHA256SUMS`.

The release assets use stable (un-versioned) names, so
`…/releases/latest/download/ganymede-bridge-esp32s3.bin` always points at the newest build —
which is what the browser flasher (`docs/flash/manifest.json`) targets.

## One-time repo setup

- **GitHub Pages** (for the browser flasher): Settings → Pages → Deploy from branch →
  `main` (or `dev`) → `/docs`. The flasher then lives at
  `https://bdherouville.github.io/pinguino-ha-remote/flash/`.
- First CI run: the **nRF job** is the only step that can't be validated locally (it bootstraps
  the NCS workspace in a container). If it fails, check the toolchain image tag / `west update`
  step; the ESP32 job uses the well-trodden `espressif/esp-idf-ci-action`.

## Building locally instead

See [`firmware/esp32_bridge/README.md`](firmware/esp32_bridge/README.md) and
[`firmware/nrf52_emulator/README.md`](firmware/nrf52_emulator/README.md).
