#include "fpga_tx.h"

#include "battery.h"
#include "brightness.h"
#include "color_correct_lcd.h"
#include "color_correct_usb.h"
#include "crc8_sae_j1850.h"
#include "dpad_ctl.h"
#include "driver/uart.h"
#include "esp_console.h"
#include "esp_err.h"
#include "esp_log.h"
#include "fpga_common.h"
#include "frameblend.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "low_batt_icon_ctl.h"
#include "palette.h"
#include "player_num.h"
#include "screen_transit_ctl.h"
#include "silent.h"
#include "style.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    kFlag_Resume,
    kFlag_SetBrightness,
    kFlag_SetSysCtl,
    kFlag_RequestFWVer,
    kFlag_PokeButton,
    kFlag_SetPaletteStyle,
    kFlag_RequestBGPD,
    kFlag_Sync,
    kNumFlags,
};

typedef enum {
    kTxFlag_Resume           = (1 << kFlag_Resume),
    kTxFlag_WriteBrightness  = (1 << kFlag_SetBrightness),
    kTxFlag_SetSysCtl        = (1 << kFlag_SetSysCtl),
    kTxFlag_RequestFWVer     = (1 << kFlag_RequestFWVer),
    kTxFlag_PokeButton       = (1 << kFlag_PokeButton),
    kTxFlag_SetPaletteStyle  = (1 << kFlag_SetPaletteStyle),
    kTxFlag_RequestBGPD      = (1 << kFlag_RequestBGPD),
    kTxFlag_Sync             = (1 << kFlag_Sync),

    // RetroPlug sync is host-driven (not part of the periodic SendAll refresh).
    kTxFlag_AllFlags         = (((1 << kNumFlags) - 1) & ~kTxFlag_Sync),
} TxFlags_t;

// Queue of RetroPlug sync payloads ({mode, serial byte} packed as a uint16),
// drained by the TX task so only it writes the FPGA UART.
#define FPGA_SYNC_QUEUE_LEN 64
static QueueHandle_t _syncQueue = NULL;

typedef enum {
    kTxCmd_Reserved0        = 0x0,
    kTxCmd_Reserved1        = 0x1,
    kTxCmd_Reserved2        = 0x2,
    kTxCmd_Reserved3        = 0x3,
    kTxCmd_SysCtrl          = 0x4,
    kTxCmd_BacklightCtl     = 0x5,
    kTxCmd_ReqFWVer         = 0x6,
    kTxCmd_PokeButton       = 0x9,
    kTxCmd_BGPaletteCtl     = 0xB,
    kTxCmd_SpritePaletteCtl = 0xC,
    kTxCmd_ReqBGPD          = 0xD,
    kTxCmd_Sync             = 0xE, // RetroPlug LSDj sync: {mode, GB-serial byte}

    kNumTxCmds,
} TxIDs_t;

// Declare a variable to hold the handle of the created event group.
static EventGroupHandle_t xEventGroupHandle;

// Declare a variable to hold the data associated with the created event group.
static StaticEventGroup_t xCreatedEventGroup;
static const char* TAG = "FpgaTx";

static size_t SetupTxBuffer(uint8_t *const pBuffer, TxIDs_t eID, uint8_t Len, void* pData);

void FPGA_TxTask(void *arg)
{
    (void)arg;

    xEventGroupHandle = xEventGroupCreateStatic( &xCreatedEventGroup );

    if (_syncQueue == NULL)
    {
        _syncQueue = xQueueCreate(FPGA_SYNC_QUEUE_LEN, sizeof(uint16_t));
    }

    FPGA_Tx_SendAll();
    FPGA_Tx_Resume();

    uint8_t TxBuffer[14] = {0};

    while (1)
    {
        //Only run when we're given permission to do so.
        (void) xEventGroupWaitBits(
            xEventGroupHandle,
            kTxFlag_Resume,
            pdFALSE,        // Do not clear the Resume flag
            pdTRUE,         // Wait for this bit
            portMAX_DELAY
        );

        const EventBits_t EventBits = xEventGroupWaitBits(
            xEventGroupHandle,
            (kTxFlag_WriteBrightness | kTxFlag_SetSysCtl | kTxFlag_RequestFWVer | kTxFlag_PokeButton | kTxFlag_SetPaletteStyle | kTxFlag_RequestBGPD | kTxFlag_Sync),
            pdTRUE, // DO clear the flags to complete the request
            pdFALSE, // Any bit will do
            pdMS_TO_TICKS(100)
        );

        if ((EventBits & kTxFlag_Sync) == kTxFlag_Sync)
        {
            // Drain the RetroPlug sync queue, injecting each byte onto the link.
            uint16_t payload;
            while ((_syncQueue != NULL) && (xQueueReceive(_syncQueue, &payload, 0) == pdPASS))
            {
                uint16_t data = payload; // {mode<<8 | serial byte}; SetupTxBuffer swaps to big-endian
                const size_t Size = SetupTxBuffer(TxBuffer, kTxCmd_Sync, sizeof(data), (void*)&data);
                (void) uart_write_bytes(UART_NUM_1, TxBuffer, Size);
            }
        }

        if ((EventBits & kTxFlag_WriteBrightness) == kTxFlag_WriteBrightness)
        {
            const uint16_t MaxDisplayBrightness = 16;
            uint16_t Backlight = (uint16_t)Brightness_GetLevel();
            if (Backlight < MaxDisplayBrightness)
            {
                const size_t Size = SetupTxBuffer(TxBuffer, kTxCmd_BacklightCtl, sizeof(Backlight), (void*)&Backlight);
                (void) uart_write_bytes(UART_NUM_1, TxBuffer, Size);
            }
        }

        if ((EventBits & kTxFlag_SetSysCtl) == kTxFlag_SetSysCtl)
        {
            const uint16_t frame_blending = (uint8_t)(FrameBlend_GetState() == kFrameBlendState_On);
            const uint16_t ismuted        = (uint8_t)(SilentMode_GetState() == kSilentModeState_On);
            const uint16_t playernum      = PlayerNum_GetNum();
            const uint16_t color_correct  = (
                ((uint16_t)(ColorCorrectLCD_GetState() == kColorCorrectLCDState_On) << 0) |
                ((uint16_t)(ColorCorrectUSB_GetState() == kColorCorrectUSBState_On) << 1)
            );
            const uint16_t IgnoreDiagonalInputs = (uint16_t)(DPadCtl_GetState() == kDPadCtlState_RejectDiag);
            const uint16_t EnableScreenTransitionFix  = (uint16_t)(ScreenTransitCtl_GetState() == kScreenTransitCtlState_On);
            const uint16_t LowBattIconControl = (uint16_t)(LowBattIconCtl_GetState());

            const uint16_t Payload = ( (frame_blending << 1) | (color_correct << 2) | ismuted | (playernum << 4) | (EnableScreenTransitionFix << 12) | (IgnoreDiagonalInputs << 11) | (LowBattIconControl << 13));
            const size_t Size = SetupTxBuffer(TxBuffer, kTxCmd_SysCtrl, sizeof(Payload), (void*)&Payload);
            (void) uart_write_bytes(UART_NUM_1, TxBuffer, Size);
        }

        if ((EventBits & kTxFlag_RequestFWVer) == kTxFlag_RequestFWVer)
        {
            uint16_t dummy = 0;
            const size_t Size = SetupTxBuffer(TxBuffer, kTxCmd_ReqFWVer, sizeof(dummy), &dummy);
            (void) uart_write_bytes(UART_NUM_1, TxBuffer, Size);
        }

        if ((EventBits & kTxFlag_PokeButton) == kTxFlag_PokeButton)
        {
            const uint16_t PokedButtons = Button_GetPokedInputs();
            const size_t Size = SetupTxBuffer(TxBuffer, kTxCmd_PokeButton, sizeof(PokedButtons), (void*)&PokedButtons);
            (void) uart_write_bytes(UART_NUM_1, TxBuffer, Size);
        }

        if ((EventBits & kTxFlag_RequestBGPD) == kTxFlag_RequestBGPD)
        {
            uint16_t dummy = 0;
            const size_t Size = SetupTxBuffer(TxBuffer, kTxCmd_ReqBGPD, sizeof(dummy), &dummy);
            (void) uart_write_bytes(UART_NUM_1, TxBuffer, Size);
        }

        if ((EventBits & kTxFlag_SetPaletteStyle) == kTxFlag_SetPaletteStyle)
        {
            if (!Style_IsInitialized())
            {
                // Prevent palette from being sent fast on initial SendAll()s
                // so that there is time to read back the hotkey data from FPGA
                vTaskDelay( pdMS_TO_TICKS(50) );
                Style_Initialize();
            }
            const StyleID_t ID = Style_GetCurrID();
            const uint64_t ColorBG = Pal_GetColor(ID, kPalette_Bg);
            // toggle custom palette enable bit
            const uint64_t PayloadBG = __builtin_bswap64(ColorBG ^ ((uint64_t)1 << kCustomPaletteEn));

            // BG
            const size_t Size = SetupTxBuffer(TxBuffer, kTxCmd_BGPaletteCtl, sizeof(PayloadBG), (void*)&PayloadBG);
            (void) uart_write_bytes(UART_NUM_1, TxBuffer, Size);

            // Sprite - Obj0
            const uint64_t ColorObj0 = Pal_GetColor(ID, kPalette_Obj0);
            const uint64_t PayloadObj0 = __builtin_bswap64(ColorObj0);
            const size_t Size2 = SetupTxBuffer(TxBuffer, kTxCmd_SpritePaletteCtl, sizeof(PayloadObj0), (void*)&PayloadObj0);
            (void)uart_write_bytes(UART_NUM_1, TxBuffer, Size2);

            // Sprite - Obj1
            const uint64_t ColorObj1 = Pal_GetColor(ID, kPalette_Obj1);
            const uint64_t PayloadObj1 = __builtin_bswap64(ColorObj1 | ((uint64_t)1 << kCustomPaletteObjSel));
            const size_t Size3 = SetupTxBuffer(TxBuffer, kTxCmd_SpritePaletteCtl, sizeof(PayloadObj1), (void*)&PayloadObj1);
            (void)uart_write_bytes(UART_NUM_1, TxBuffer, Size3);
        }

        memset(TxBuffer, 0x0, sizeof(TxBuffer));
    }

    ESP_LOGE(TAG, "TxTask loop exited");
}

void FPGA_Tx_Resume(void)
{
   (void) xEventGroupSetBits(xEventGroupHandle, kTxFlag_Resume);
}

void FPGA_Tx_Pause(void)
{
   (void) xEventGroupClearBits(xEventGroupHandle, kTxFlag_Resume);
}

void FPGA_Tx_SendAll(void)
{
   (void) xEventGroupSetBits(xEventGroupHandle, kTxFlag_AllFlags);
}

void FPGA_Tx_WriteBrightness(void)
{
   (void) xEventGroupSetBits(xEventGroupHandle, kTxFlag_WriteBrightness);
}

void FPGA_Tx_SendSysCtl(void)
{
   (void) xEventGroupSetBits(xEventGroupHandle, kTxFlag_SetSysCtl);
}

void FPGA_Tx_PokeButtons(void)
{
   (void) xEventGroupSetBits(xEventGroupHandle, kTxFlag_PokeButton);
}

void FPGA_Tx_WritePaletteStyle(void)
{
   (void) xEventGroupSetBits(xEventGroupHandle, kTxFlag_SetPaletteStyle);
}

void FPGA_Tx_SendSync(uint8_t mode, uint8_t serial_byte)
{
    if (_syncQueue == NULL)
    {
        return;
    }
    const uint16_t payload = ((uint16_t)mode << 8) | serial_byte;
    if (xQueueSend(_syncQueue, &payload, 0) == pdPASS)
    {
        (void) xEventGroupSetBits(xEventGroupHandle, kTxFlag_Sync);
    }
    else
    {
        ESP_LOGW(TAG, "sync queue full; dropped byte 0x%02x", serial_byte);
    }
}

// Console command: "rpsync <mode_hex> <byte_hex> [byte_hex ...]"
// The RetroPlug host bridge sends this over the CDC/console to stream LSDj sync
// bytes. Mirrors the "poke" button command pattern.
static int rpsync_command(int argc, char **argv)
{
    if (argc < 3)
    {
        ESP_LOGE(TAG, "rpsync expects <mode_hex> <byte_hex> [byte_hex ...]");
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t mode = (uint8_t)strtol(argv[1], NULL, 16);
    for (int i = 2; i < argc; i++)
    {
        const uint8_t b = (uint8_t)strtol(argv[i], NULL, 16);
        FPGA_Tx_SendSync(mode, b);
    }
    return ESP_OK;
}

void FPGA_Tx_RegisterCommands(void)
{
    esp_console_cmd_t command = {
        .command = "rpsync",
        .help = "RetroPlug: inject LSDj sync bytes onto the Game Boy link",
        .func = &rpsync_command,
        .argtable = NULL,
    };

    esp_err_t err;
    if ((err = esp_console_cmd_register(&command)) != ESP_OK)
    {
        ESP_LOGE(TAG, "Registering '%s' command failed: %s", command.command, esp_err_to_name(err));
    }
}

static size_t SetupTxBuffer(uint8_t *const pBuffer, TxIDs_t eID, uint8_t Len, void* pData)
{
    if ((pBuffer == NULL) || (Len > kSysMgmtConsts_MsgProtoV2Len) || (pData == NULL) || ((unsigned)eID >= kNumTxCmds))
    {
        return 0;
    }

    pBuffer[0] = (uint8_t)(FPGA_IsProtoV1() ? kSysMgmtConsts_HeaderV1Marker : kSysMgmtConsts_HeaderV2Marker);
    pBuffer[1] = (uint8_t)eID;

    size_t MsgSize = 4;
    size_t PayloadOffset = 2;
    if (pBuffer[0] == kSysMgmtConsts_HeaderV2Marker)
    {
        PayloadOffset++;
    }

    if ((pData != NULL) && (Len > 0))
    {
        // 16-bit data is sent in big endian
        if (Len == 2)
        {
            uint16_t *const  pu16Data = (uint16_t *const)pData;
            *pu16Data = __builtin_bswap16(*pu16Data);
        }

        memcpy(&pBuffer[PayloadOffset], pData, Len);
    }

    if (pBuffer[0] == kSysMgmtConsts_HeaderV2Marker)
    {
        pBuffer[2] = Len;
        MsgSize = crc8_sae_j1850_encode(pBuffer, 3 + Len, pBuffer);
    }

    return MsgSize;
}
