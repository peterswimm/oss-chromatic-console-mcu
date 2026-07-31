#pragma once

#include <stdint.h>

typedef enum {
    kFPGA_TxConsts_BufferSize = 1024,    // [bytes]
} FPGA_TxConsts_t;

void FPGA_TxTask(void *arg);
void FPGA_Tx_Resume(void);
void FPGA_Tx_Pause(void);
void FPGA_Tx_SendAll(void);
void FPGA_Tx_WriteBrightness(void);
void FPGA_Tx_SendSysCtl(void);
void FPGA_Tx_PokeButtons(void);
void FPGA_Tx_WritePaletteStyle(void);

// RetroPlug LSDj sync (host -> FPGA). Queues one {mode, GB-serial byte} to be
// injected onto the Game Boy link by the FPGA linksync block (RX address 0xE).
// The host (RetroPlug, running Ableton Link) streams these over the USB CDC /
// console; see FPGA_Tx_RegisterCommands (the "rpsync" console command).
void FPGA_Tx_SendSync(uint8_t mode, uint8_t serial_byte);

// Register the "rpsync" console command used by the host bridge.
void FPGA_Tx_RegisterCommands(void);