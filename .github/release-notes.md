Unbrick an M-VAVE FM-1 or SMK-37 with only an M5Stack Cardputer ADV and a USB-C cable.

**Download** `synth-rescue-v{{VERSION}}-cardputer-adv.bin`: one merged image (bootloader, partitions, app), at `0x0`. `SHA256SUMS` has its checksum.

**Install**
- From an SD card with [M5Launcher](https://github.com/bmorcelli/Launcher): copy the `.bin` to the card (FAT32, 32 GB or less), then in Launcher: SD, the file, Install.
- With esptool: the Cardputer in download mode (power switch OFF, hold G0, plug in USB-C), then
  `esptool --chip esp32s3 write-flash 0x0 synth-rescue-v{{VERSION}}-cardputer-adv.bin`

**Use**: it starts in the guided mode. Switch the synth off (1), connect it with a USB-C cable (2), an SD card for a backup is optional (3), switch the synth on, and follow the screen. The repair (Y) installs a fresh m-bootloader and erases the old firmware: the synth then waits in MIDI mode, and you install a firmware with its own installer from a phone or computer. The advanced mode (A) has the log, self-tests and a hand-off to a PC.
