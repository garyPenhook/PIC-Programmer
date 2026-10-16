# picprog

Host tool for the NUCLEO-G071RB PIC ICSP programmer. It drives the firmware's serial
command interface (`include/cmd.h` in the repository) over the ST-LINK virtual COM port
and converts between the target's memory and Intel HEX files.

Install it as a command with uv, from this directory:

```sh
uv tool install .
picprog --port /dev/ttyACM0 probe
```

See the repository README ("Host tool") for the commands and the hex file layout.
