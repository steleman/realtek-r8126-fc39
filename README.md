# Realtek r8126 Ethernet driver for Linux Kernels >= 6.9

This is a port of the r8216 Realetk Ethernet driver that compiles and works with Linux Kernels >= 6.9.X.
It is based on the "official" Realtek driver as published by Realtek.

The bulk of the changes are in `src/r8126_n.c` and `rtl_eeprom.c`.

## Installation

You can build and install it from source with DKMS, following the standard DKMS installation steps:
  - Clone this repository.
  - rm -rf the `.git` directory
  - rename this directory to `r8126-10.013.00`
  - copy the entire renamed directory to `/usr/src/` (you need root to do this)
  - do `sudo dkms add r8126/10.013.00`
  - followed by `sudo dkms install r8126/10.013.00`

That's it. On Fedora at least, `dkms install` doesn't actually install the module where it's set to go in
`dkms.conf`. It installs it instead in `/lib/modules/$(uname -r)/extra/`.

That's not necessarily what we want. In fact, Realtek Ethernet drivers should go in
`/lib/modules/$(uname -r)/kernel/drivers/net/ethernet/realtek/`.
