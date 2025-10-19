get an iso with "make iso", run it automatically in qemu with "make run", or if you just want the kernel.elf run "make all". 
"make debug" starts qemu with -s -S, so you can connect with a GDB command like:<br>
`gdb -ex "target remote localhost:1234" -ex "symbol-file out/kernel.elf"`