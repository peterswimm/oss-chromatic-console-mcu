# RetroPlug LSDj sync relay (MCU side)

This firmware relays LSDj sync between a host and the FPGA, so a DAW / Ableton
Link session can drive LSDj tempo on the Chromatic. It is the MCU half of the
[RetroPlug port](https://github.com/peterswimm/oss-chromatic-console-fpga/tree/main/docs/retroplug-port);
the host bridge lives in [RetroPlug](https://github.com/peterswimm/retroplug) and
the injector/capture RTL in the FPGA repo.

> **Proof-of-concept.** Not hardware-validated. Requires FPGA **v19.0+**.

## Data flow

```
RetroPlug (host, runs Ableton Link, emits LSDj sync bytes)
   │  USB CDC / console  (rpsync command, and RPMIDI: lines back)
   ▼
ESP32 MCU (this firmware)
   │  0x8F V2 UART packet protocol
   ▼
FPGA  linksync (RX 0xE -> inject onto GB link)  /  linkcapture (TX 0xA -> byte)
   ▼
LSDj (external-clock slave)
```

The MCU does **not** run Ableton Link or any network stack — it is a thin relay.
On-device Link is future work (see the FPGA repo's `docs/retroplug-port/ableton-link.md`).

## Host → device: the `rpsync` console command

The host bridge streams sync bytes by issuing a console command over the CDC /
console UART (the same REPL used for the existing `poke` button command):

```
rpsync <mode_hex> <byte_hex> [byte_hex ...]
```

- `mode_hex` — the `LsdjSyncMode` (matches RetroPlug's `settingsEnums.ts`):
  e.g. midiSync, midiSyncArduinoboy, midiMap, midiPassthrough.
- `byte_hex …` — one or more LSDj/MIDI serial bytes to inject, in order.

Each byte is queued (`FPGA_Tx_SendSync`) and the TX task emits a `0x8F` V2 packet
to FPGA RX address **0xE** with payload `{mode, byte}` (16-bit, big-endian, as the
protocol expects). The FPGA `linksync` block shifts it onto the Game Boy link.

> A console command is the PoC transport (idiomatic, matches `poke`). For
> sustained real-time streaming a dedicated raw-UART mode would be added later;
> the byte queue + packet path is unchanged by that.

## Device → host: MIDI-out / MasterSync

When LSDj is the serial master (MasterSync / Arduinoboy MI.OUT), the FPGA
`linkcapture` block deserializes the link and reports each byte to the MCU on a
new FPGA→MCU TX channel (**RX command 0xA**, `kRxCmd_MidiOut`). The MCU forwards
it to the host as a labeled console line:

```
RPMIDI:<hex>
```

RetroPlug parses these back into MIDI (its `ArduinoboyMaster` / `masterSync`
roles). A raw binary channel would be cleaner long-term; the labeled line keeps
the PoC compatible with the existing REPL.

## Files

- `main/fpga_tx.c` / `.h` — `FPGA_Tx_SendSync`, the sync queue, and the `rpsync`
  console command (`FPGA_Tx_RegisterCommands`, registered in `main/main.c`).
- `main/fpga_rx.c` / `.h` — `kRxCmd_MidiOut` handler.
