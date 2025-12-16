The files in this directory are tools to extract the firmware for
Focusrite Scarlett 4th generation big devices (16i16, 18i16, and
18i20) from a USB capture of the firmware update process performed by
FC2. You don't need anything in this directory unless you are the
maintainer of fcp-server adding support for a new firmware version.

1. **Start the capture** (before you connect the device to the VM):
   ```
   BUS=$(lsusb -d1235: | grep -oP 'Bus 0*\K\d+')
   PID=$(lsusb -d1235: | grep -o 82..)
   NOW=$(date +%Y%m%d-%H%M%S)
   usbmon -i $BUS -s 65536 -fu > scarlett4-$PID-$NOW.cap
   ```

2. **Run the update:** connect the device to the VM and start the
   update. Re-connect it each time it disconnects.

3. **Stop the capture:** when the update is complete, disconnect the
   device from the VM and stop the capture (Ctrl+C).

4. **Extract the firmware:**
   ```
   usbcap-to-scarlett4 < scarlett4-$PID-$NOW.cap
   ```
   Sample output:
   ```
   firmware boundary: leapfrog -> esp
   firmware boundary: esp -> app
   extracted leapfrog firmware, size: 244736 bytes
   extracted esp firmware, size: 1445888 bytes
   extracted app firmware, size: 261376 bytes
   wrote scarlett4-1235-821b-unk.bin
   ```

5. **Install the leapfrog firmware:**
   ```
   fcp-tool upload-leapfrog -f scarlett4-1235-$PID-unk.bin
   ```

6. **Reboot** into the leapfrog firmware:
   ```
   fcp-tool reboot
   ```

7. **Get the leapfrog version** and update the firmware file:
   ```
   scarlett4-ver-update scarlett4-1235-$PID-unk.bin
   ```
   Sample output:
   ```
   amixer: Cannot find the given element from control sysdefault:1

   Current versions:
   App: 0 1 2132 4591
   ESP: N/A (Leapfrog mode)
   Container VID/PID: 1235:821X
   Found section: SCARLEAP
   Section VID/PID: 1235:821X
   Updating section, new version: 0 1 2132 4591
   Found section: SCARLESP
   Section VID/PID: 1235:821X
   Skipping section
   Found section: SCARLET4
   Section VID/PID: 1235:821X
   Skipping section
   ```

8. **Install the ESP firmware:**
   ```
   fcp-tool upload-esp -f scarlett4-1235-$PID-unk.bin
   ```

9. **Install the app firmware:**
   ```
   fcp-tool update -f scarlett4-1235-$PID-unk.bin
   ```

10. **Get the ESP and app versions** and update the firmware file:
   ```
   scarlett4-ver-update scarlett4-1235-$PID-unk.bin
   ```
   Sample output:
   ```
   Current versions:
   App: 2 0 2426 0
   ESP: 1 0 0 349
   Container VID/PID: 1235:821X
   Found section: SCARLEAP
   Section VID/PID: 1235:821X
   Skipping section
   Found section: SCARLESP
   Section VID/PID: 1235:821X
   Updating section, new version: 1 0 0 349
   Found section: SCARLET4
   Section VID/PID: 1235:821X
   Updating section, new version: 2 0 2426 0

   Suggested rename command:
   mv scarlett4-1235-821b-unk.bin scarlett4-1235-821b-2464.bin
   ```

11. **Rename the file** as suggested.

12. **Test it:** downgrade to a previous version, confirm that FC2
    wants to update it (but don't let it), then upgrade to the
    extracted version using fcp-tool and confirm that FC2 now doesn't
    prompt to update it.
