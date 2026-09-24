# nb2_immobilizer_delete

This board is meant to be installed on the connector pins inside the ECU and disable the immobilizer by providing affirmative responses even when a valid chip is not present.

To use, remove the ecu from the car and open the lid. Align the board with the main connector and solder the 3 circled points, then close and reinstall.

Leaving the factory immobilizer connected, turn the key to the on position and wait for a few seconds. Then turn the key off and disconnect the factory immobilizer module under the steering column.

The car should now start with any working key, regardless of chip pairing.


The rough design and firmware were taken from a miata.net thread, credit to Alcantor

https://forum.miata.net/vb/showthread.php?p=8561544#post8561544

# Build and flash instructions

This firmware targets a **bare ATtiny85 running at 1 MHz** using the calibrated 8 MHz internal RC oscillator with the `CKDIV8` fuse programmed.

## Toolchain

Install these tools:

- `avr-gcc`
- `avr-libc`
- `binutils-avr` / `avr-objcopy`
- `avrdude`

On Debian/Ubuntu/WSL:

```bash
sudo apt update
sudo apt install gcc-avr avr-libc binutils-avr avrdude
```

## Build

From the repository root:

```bash
avr-gcc \
  -Wall -Wextra -Os -std=gnu11 \
  -ffunction-sections -fdata-sections \
  -Wl,--gc-sections \
  -DF_CPU=1000000UL \
  -mmcu=attiny85 \
  -o main.elf main.c

avr-objcopy -j .text -j .data -O ihex main.elf main.hex
avr-objcopy -j .text -j .data -O binary main.elf main.bin
avr-size -C --mcu=attiny85 main.elf
```

`main.hex` is the recommended file for AVRDUDE. `main.bin` is also produced as a raw flash image for releases or other programming tools.

## 1 MHz clock fuse

Use low fuse **`0x62`**:

- internal calibrated 8 MHz RC oscillator
- `CKDIV8` programmed, giving a 1 MHz system clock
- clock output disabled
- default long startup delay retained

Only the low fuse needs to be changed for the 1 MHz clock configuration. Do **not** program `RSTDISBL`; the RESET pin is required for ISP.

You can read the existing fuses before changing anything:

```bash
avrdude -c arduino_as_isp -P COM5 -b 19200 -p t85 \
  -U lfuse:r:-:h -U hfuse:r:-:h -U efuse:r:-:h
```

Replace `COM5` with the programmer's serial port. On Linux/WSL this may be `/dev/ttyACM0` or `/dev/ttyUSB0`.

## Arduino as ISP

Load the standard **ArduinoISP** sketch onto the Arduino programmer first. The current ArduinoISP sketch uses a target SPI clock of approximately 166.7 kHz by default, specifically chosen to be safe for a 1 MHz ATtiny target.

### AVRDUDE 8.x / current programmer name

Set the 1 MHz fuse:

```bash
avrdude -c arduino_as_isp -P COM5 -b 19200 -p t85 \
  -U lfuse:w:0x62:m
```

Flash and verify the HEX file:

```bash
avrdude -c arduino_as_isp -P COM5 -b 19200 -p t85 \
  -U flash:w:main.hex:i \
  -U flash:v:main.hex:i
```

Or write the fuse and firmware in one command:

```bash
avrdude -c arduino_as_isp -P COM5 -b 19200 -p t85 \
  -U lfuse:w:0x62:m \
  -U flash:w:main.hex:i \
  -U flash:v:main.hex:i
```

### Older AVRDUDE installations

If your AVRDUDE does not recognize `arduino_as_isp`, use the STK500v1 programmer definition used by the ArduinoISP sketch:

```bash
avrdude -c stk500v1 -P COM5 -b 19200 -p t85 \
  -U lfuse:w:0x62:m \
  -U flash:w:main.hex:i \
  -U flash:v:main.hex:i
```

The `-b 19200` option is the serial link speed between the PC and the Arduino running ArduinoISP; it is **not** the ISP SCK frequency sent to the ATtiny.

## ISP wiring

ATtiny85 ISP signals are:

| ATtiny85 | ISP function |
|---|---|
| PB0 / pin 5 | MOSI |
| PB1 / pin 6 | MISO |
| PB2 / pin 7 | SCK |
| PB5 / pin 1 | RESET |
| pin 8 | VCC |
| pin 4 | GND |

The application signals are deliberately moved away from the ISP pins:

- `TX_INV` = PB3 / pin 2
- `RX_INV` = PB4 / pin 3

`RX_INV` has an external 10 kOhm pull-up, so the firmware does not enable the ATtiny internal pull-up on PB4.

## EEPROM note

The firmware stores the learned five-byte immobilizer response in EEPROM. A normal AVRDUDE flash operation may perform a chip erase, and the factory-default high fuse does **not** preserve EEPROM through chip erase.

If you intend to reflash a unit after it has learned a code, back up EEPROM first:

```bash
avrdude -c arduino_as_isp -P COM5 -b 19200 -p t85 \
  -U eeprom:r:eeprom-backup.hex:i
```

Restore it afterward if necessary:

```bash
avrdude -c arduino_as_isp -P COM5 -b 19200 -p t85 \
  -U eeprom:w:eeprom-backup.hex:i
```

Alternatively, the `EESAVE` high-fuse bit can be programmed so EEPROM survives chip erase, but that changes the high fuse and should be done deliberately rather than as part of the basic 1 MHz flashing command.
