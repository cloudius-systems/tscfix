tscfix - Fix Time-Stamp counter on x86 machines with a bad BIOS
======

Introduction
------------

Some x86 machines (e.g. the notorious DX79TO) have a BIOS bug that scrambles their
time-stamp counter (TSC) even though their processor's TSC is otherwise reliable.
This forces the kernel to use slower time sources, such as HPET.

tscfix attempts to work around this issue by re-synchronizing the TSC.

How to use
----------

 1. Build and install [tscadj](https://github.com/cloudius-systems/tscadj)
 2. Build tscfix (```make```)
 3. Run ```tscfix``` once to view tsc skew
 4. Run ```tscfix -f``` as root to attempt to correct the skew
 5. Run ```tscfix``` again to verify that the skew has been eliminated.  A few dozen cycles
    of skew is permissable.
 6. Use ```kexec``` to reboot your kernel without going through the BIOS:
    ```
    kexec -l /path/to/kernel --initrd /path/to/initrd --reuse-cmdline --append tsc=reliable
    kexec -e
    ```
    (as root)
    
