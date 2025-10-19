# trisys64
Trisys is a novel, x86_64 kernel written in C++. More information to come

## Building
### ISO
To get an ISO that runs on 64-bit machines via legacy or UEFI boot, run `make iso`. An iso file will be produced in out/trisys.iso by default.
### QEMU
To build the ISO and automatically run it in QEMU with OVMF firmware, run `make run`. This might require some tweaking as my machine uses Arch, which has a nonstandard location for OVMF firmware. It can also be run manually without providing an OVMF path to use legacy boot.
### Raw .elf
To get only the raw kernel.elf file, you can run `make all`. This will produce out/kernel.elf, which is not very useful on its own, but I don't know your motivations.
### Debugging
You can use `make debug` to run in EFI mode with QEMU's debug mode enabled, allowing you to attach a debugger with e.g. `gdb -ex "target remote localhost:1234" -ex "symbol-file out/kernel.elf"`. The same considerations apply as with normal `make run.`
### Optimized builds
There's no support right now for optimized builds out of the box, but you can run something like `make clean all EXTRA_CFLAGS="-O3 -DNDEBUG=1"` to pass -O3 -DNDEBUG=1 into CFLAGs.
