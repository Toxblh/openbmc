# p2a: reading and resetting a hung BMC from the Elbrus host

The AST2500's P2A bridge (PCIe -> AHB, through the ASPEED VGA's BAR1,
0000:05:00.0 on the host) is open whenever the BMC's Linux hasn't closed it:
in U-Boot it always is. Root on the host reads/writes all of the BMC's
memory and registers.

Build and run on the host (HOST: its address, USER: a user with sudo there), root needed:

    scp p2a.c README.md USER@HOST:bmc-rescue/ && ssh USER@HOST 'cd bmc-rescue && gcc -O2 -Wall -o p2a p2a.c'
    ssh -t USER@HOST 'sudo ~/bmc-rescue/p2a diag'

    p2a diag                 SoC id, WDT1-3 (counter twice, 2 s apart), SCU reset/strap, UART5 LSR
    p2a rd ADDR [N]          N words
    p2a find ADDR LEN TEXT   search BMC memory for TEXT
    p2a dump ADDR LEN FILE   raw bytes
    p2a wdtreset SECONDS     WDT2: reset the BMC SoC in SECONDS (asks "yes")

## Reading `diag`

U-Boot stopped at its prompt (REGRESSIONS 135: a byte on the console in
the bootdelay window): every WDT ctrl has bit 0 clear (off) and the
counters don't move in 2 s. A running BMC Linux: WDT1 on, counter moving.

## Resetting the BMC

    ssh -t USER@HOST 'sudo ~/bmc-rescue/p2a wdtreset 5'   # type yes

WDT2 at 1 MHz, ctrl 0x13 (enable, reset, SoC reset mode). WDT2 boots from
the primary flash (WDT1 has the second-flash bit: never use it for this).
2026-10-21: BMC reboots and WDT resets hard-reset the host (twice).
2026-11-08: the host survived the BMC's reboot into beta6. Not settled:
expect the host may go down.

## Writing the BMC flash (a broken U-Boot, no programmer)

    sudo ~/bmc-rescue/p2a flash-id                         # the chip answers: 20ba20 (N25Q512)
    sudo ~/bmc-rescue/p2a flash-read 0 0xe0000 uboot.bin   # read (the memory window, nothing switched)
    sudo rmmod ast 2>/dev/null                             # the host's ast driver moves the bridge's window
    sudo ~/bmc-rescue/p2a flash-write 0 u-boot.bin         # only the 4 KiB blocks that differ, read back

FMC user mode as Linux does it (CE0 control 0x7/0x3, bytes through the CE0
window 0x20000000), 21h erase and 12h program with 4-byte addresses, Micron
flag status (70h). Refused while the BMC runs (WDT1 counting) or the ast
driver is loaded, unless --force. The same code built -DDEVMEM runs on the
BMC over /dev/mem: test_regressions_qemu p2aflash writes a U-Boot with it and
boots QEMU from the result.
