/*
 * mcp2515.c - minimal MCP2515 driver for the inverter bus.
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * One task owns the SPI device and does all chip access: it is woken by the INT
 * pin (and polls every few ms as a fallback), drains both RX buffers into the
 * bridge queue and feeds TX buffer 0 from a FIFO. Only TXB0 is used so frames
 * leave in exactly the order the bridge produced them, which also sidesteps the
 * MCP2515/MCP25625 multi-buffer priority errata v1.x worked around.
 */
#include "mcp2515.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "rom/ets_sys.h"
#include "sdkconfig.h"

static const char *TAG = "mcp2515";

/* Instructions */
#define INS_RESET       0xC0
#define INS_READ        0x03
#define INS_WRITE       0x02
#define INS_BIT_MODIFY  0x05
#define INS_READ_STATUS 0xA0
#define INS_LOAD_TXB0   0x40
#define INS_RTS_TXB0    0x81
#define INS_READ_RXB0   0x90
#define INS_READ_RXB1   0x94

/* Registers */
#define REG_CANSTAT     0x0E
#define REG_CANCTRL     0x0F
#define REG_CNF3        0x28
#define REG_CNF2        0x29
#define REG_CNF1        0x2A
#define REG_CANINTE     0x2B
#define REG_CANINTF     0x2C
#define REG_EFLG        0x2D
#define REG_TXB0CTRL    0x30
#define REG_RXB0CTRL    0x60
#define REG_RXB1CTRL    0x70

/* Bits */
#define INT_RX0         0x01
#define INT_RX1         0x02
#define INT_TX0         0x04
#define INT_ERR         0x20
#define INT_MERR        0x80
#define STAT_RX0IF      0x01
#define STAT_RX1IF      0x02
#define STAT_TX0REQ     0x04
#define STAT_TX0IF      0x08
#define EFLG_TXBO       0x20
#define EFLG_RX0OVR     0x40
#define EFLG_RX1OVR     0x80
#define TXB_TXREQ       0x08
#define MODE_MASK       0xE0
#define MODE_NORMAL     0x00
#define MODE_CONFIG     0x80

#define TX_QUEUE_DEPTH  32
#define TX_TIMEOUT_US   (50 * 1000)
#define EFLG_PERIOD_US  (100 * 1000)
#define TASK_STACK      3072
#define TASK_PRIO       (configMAX_PRIORITIES - 2) /* just above the bridge task */
#define TASK_CORE       1

/* 500 kbit/s bit timing (CNF1, CNF2, CNF3), BRP = 0, SJW = 1 TQ. */
#if CONFIG_BRIDGE_MCP2515_CRYSTAL_16MHZ
/* 16 TQ of 125 ns: sync 1 + prop 6 + PS1 7 + PS2 2 -> sample point 87.5 % */
static const uint8_t k_cnf[3] = {0x00, 0xB5, 0x01};
#define CRYSTAL_NAME "16 MHz"
#else
/* 8 TQ of 250 ns: sync 1 + prop 2 + PS1 3 + PS2 2 -> sample point 75 % */
static const uint8_t k_cnf[3] = {0x00, 0x91, 0x01};
#define CRYSTAL_NAME "8 MHz"
#endif

static spi_device_handle_t s_spi;
static TaskHandle_t s_task;
static QueueHandle_t s_tx_queue;
static bridge_bus_stats_t s_stats;

/* ---- SPI primitives --------------------------------------------------------------- */
static esp_err_t xfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    return spi_device_polling_transmit(s_spi, &t);
}

static uint8_t read_reg(uint8_t reg)
{
    const uint8_t tx[3] = {INS_READ, reg, 0};
    uint8_t rx[3] = {0};
    (void)xfer(tx, rx, sizeof(tx));
    return rx[2];
}

static void write_reg(uint8_t reg, uint8_t value)
{
    const uint8_t tx[3] = {INS_WRITE, reg, value};
    (void)xfer(tx, NULL, sizeof(tx));
}

static void bit_modify(uint8_t reg, uint8_t mask, uint8_t value)
{
    const uint8_t tx[4] = {INS_BIT_MODIFY, reg, mask, value};
    (void)xfer(tx, NULL, sizeof(tx));
}

static uint8_t read_status(void)
{
    const uint8_t tx[2] = {INS_READ_STATUS, 0};
    uint8_t rx[2] = {0};
    (void)xfer(tx, rx, sizeof(tx));
    return rx[1];
}

/* ---- frames ------------------------------------------------------------------------- */
/* READ RX BUFFER returns SIDH SIDL EID8 EID0 DLC D0..D7 and clears RXnIF. */
static void read_rx_buffer(uint8_t instruction)
{
    uint8_t tx[14] = {instruction};
    uint8_t rx[14] = {0};
    if (xfer(tx, rx, sizeof(tx)) != ESP_OK) {
        return;
    }
    const uint8_t sidh = rx[1], sidl = rx[2], eid8 = rx[3], eid0 = rx[4], dlc = rx[5];
    leaf_can_frame_t f;
    memset(&f, 0, sizeof(f));
    if (sidl & 0x08) { /* IDE */
        f.id = ((uint32_t)sidh << 21) | ((uint32_t)(sidl & 0xE0) << 13) | ((uint32_t)(sidl & 0x03) << 16) |
               ((uint32_t)eid8 << 8) | eid0;
        f.flags |= LEAF_CAN_FLAG_EXTENDED;
        if (dlc & 0x40) {
            f.flags |= LEAF_CAN_FLAG_RTR;
        }
    } else {
        f.id = ((uint32_t)sidh << 3) | (sidl >> 5);
        if (sidl & 0x10) { /* SRR = standard remote frame */
            f.flags |= LEAF_CAN_FLAG_RTR;
        }
    }
    f.dlc = (uint8_t)((dlc & 0x0F) > LEAF_CAN_MAX_DLC ? LEAF_CAN_MAX_DLC : (dlc & 0x0F));
    if (!(f.flags & LEAF_CAN_FLAG_RTR)) {
        memcpy(f.data, &rx[6], f.dlc);
    }
    s_stats.rx++;
    bridge_post(LEAF_BUS_INVERTER, &f);
}

static void load_and_send_txb0(const leaf_can_frame_t *f)
{
    uint8_t tx[14] = {INS_LOAD_TXB0};
    if (leaf_can_frame_is_extended(f)) {
        tx[1] = (uint8_t)(f->id >> 21);
        tx[2] = (uint8_t)(((f->id >> 13) & 0xE0) | 0x08 | ((f->id >> 16) & 0x03));
        tx[3] = (uint8_t)(f->id >> 8);
        tx[4] = (uint8_t)f->id;
    } else {
        tx[1] = (uint8_t)(f->id >> 3);
        tx[2] = (uint8_t)((f->id & 0x07) << 5);
    }
    tx[5] = (uint8_t)(f->dlc | (leaf_can_frame_is_rtr(f) ? 0x40 : 0x00));
    memcpy(&tx[6], f->data, f->dlc);
    (void)xfer(tx, NULL, 6u + f->dlc);
    const uint8_t rts = INS_RTS_TXB0;
    (void)xfer(&rts, NULL, 1);
}

/* ---- service task -------------------------------------------------------------------- */
static void IRAM_ATTR int_isr(void *arg)
{
    (void)arg;
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_task, &woken);
    if (woken) {
        portYIELD_FROM_ISR();
    }
}

static void service_task(void *arg)
{
    (void)arg;
    bool tx_busy = false;
    int64_t tx_started = 0;
    int64_t next_eflg = 0;
    bool was_bus_off = false;
    leaf_can_frame_t f;

    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2));

        for (int guard = 0; guard < 32; guard++) {
            const uint8_t status = read_status();
            bool worked = false;
            if (status & STAT_RX0IF) { /* RXB0 holds the older frame when both are full */
                read_rx_buffer(INS_READ_RXB0);
                worked = true;
            }
            if (status & STAT_RX1IF) {
                read_rx_buffer(INS_READ_RXB1);
                worked = true;
            }
            if (tx_busy && (status & STAT_TX0IF)) {
                bit_modify(REG_CANINTF, INT_TX0, 0);
                tx_busy = false;
                s_stats.tx++;
                worked = true;
            }
            if (!tx_busy && xQueueReceive(s_tx_queue, &f, 0) == pdTRUE) {
                load_and_send_txb0(&f);
                tx_busy = true;
                tx_started = esp_timer_get_time();
                worked = true;
            }
            if (!worked) {
                break;
            }
        }

        const int64_t now = esp_timer_get_time();
        if (tx_busy && now - tx_started > TX_TIMEOUT_US) {
            /* Not acknowledged (inverter off / bus unplugged): abort so the queue keeps moving. */
            bit_modify(REG_TXB0CTRL, TXB_TXREQ, 0);
            bit_modify(REG_CANINTF, INT_TX0, 0);
            tx_busy = false;
            s_stats.tx_dropped++;
        }
        if (now >= next_eflg) {
            next_eflg = now + EFLG_PERIOD_US;
            const uint8_t eflg = read_reg(REG_EFLG);
            if (eflg & (EFLG_RX0OVR | EFLG_RX1OVR)) {
                s_stats.rx_overflow++;
                bit_modify(REG_EFLG, EFLG_RX0OVR | EFLG_RX1OVR, 0);
            }
            const bool bus_off = (eflg & EFLG_TXBO) != 0;
            if (bus_off && !was_bus_off) {
                s_stats.bus_off++; /* the MCP2515 recovers on its own */
                ESP_LOGW(TAG, "bus-off");
            }
            was_bus_off = bus_off;
            const uint8_t intf = read_reg(REG_CANINTF);
            if (intf & (INT_ERR | INT_MERR)) {
                bit_modify(REG_CANINTF, INT_ERR | INT_MERR, 0);
            }
        }
    }
}

/* ---- setup ------------------------------------------------------------------------------ */
static esp_err_t set_mode(uint8_t mode)
{
    bit_modify(REG_CANCTRL, MODE_MASK, mode);
    for (int i = 0; i < 20; i++) {
        if ((read_reg(REG_CANSTAT) & MODE_MASK) == mode) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t mcp2515_start(void)
{
    const spi_bus_config_t bus = {
        .sclk_io_num = CONFIG_BRIDGE_MCP2515_SCK_GPIO,
        .mosi_io_num = CONFIG_BRIDGE_MCP2515_MOSI_GPIO,
        .miso_io_num = CONFIG_BRIDGE_MCP2515_MISO_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 32,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_DISABLED);
    if (err != ESP_OK) {
        return err;
    }
    const spi_device_interface_config_t dev = {
        .mode = 0,
        .clock_speed_hz = CONFIG_BRIDGE_MCP2515_SPI_HZ,
        .spics_io_num = CONFIG_BRIDGE_MCP2515_CS_GPIO,
        .queue_size = 1,
    };
    err = spi_bus_add_device(SPI2_HOST, &dev, &s_spi);
    if (err != ESP_OK) {
        return err;
    }
    /* Only device on this bus: keep it acquired for low-latency polling transfers. */
    err = spi_device_acquire_bus(s_spi, portMAX_DELAY);
    if (err != ESP_OK) {
        return err;
    }

    const uint8_t reset = INS_RESET;
    (void)xfer(&reset, NULL, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    const uint8_t canstat = read_reg(REG_CANSTAT);
    if ((canstat & MODE_MASK) != MODE_CONFIG) {
        ESP_LOGE(TAG, "no MCP2515 found (CANSTAT 0x%02X) - check wiring / power", canstat);
        return ESP_ERR_NOT_FOUND;
    }

    write_reg(REG_CNF1, k_cnf[0]);
    write_reg(REG_CNF2, k_cnf[1]);
    write_reg(REG_CNF3, k_cnf[2]);
    if (read_reg(REG_CNF2) != k_cnf[1]) {
        ESP_LOGE(TAG, "register write verification failed");
        return ESP_ERR_INVALID_RESPONSE;
    }
    write_reg(REG_RXB0CTRL, 0x64); /* receive any message, roll over into RXB1 */
    write_reg(REG_RXB1CTRL, 0x60); /* receive any message */
    write_reg(REG_CANINTF, 0x00);
    write_reg(REG_CANINTE, INT_RX0 | INT_RX1 | INT_TX0);

    s_tx_queue = xQueueCreate(TX_QUEUE_DEPTH, sizeof(leaf_can_frame_t));
    if (s_tx_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(service_task, "mcp2515", TASK_STACK, NULL, TASK_PRIO, &s_task, TASK_CORE) !=
        pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_BRIDGE_MCP2515_INT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { /* already installed is fine */
        return err;
    }
    ESP_ERROR_CHECK(gpio_isr_handler_add(CONFIG_BRIDGE_MCP2515_INT_GPIO, int_isr, NULL));

    err = set_mode(MODE_NORMAL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not enter normal mode");
        return err;
    }
    s_stats.running = true;
    xTaskNotifyGive(s_task);
    ESP_LOGI(TAG, "inverter bus running (%s crystal, SPI %d Hz)", CRYSTAL_NAME, CONFIG_BRIDGE_MCP2515_SPI_HZ);
    return ESP_OK;
}

bool mcp2515_send(const leaf_can_frame_t *frame)
{
    if (!s_stats.running || xQueueSend(s_tx_queue, frame, 0) != pdTRUE) {
        s_stats.tx_dropped++;
        return false;
    }
    xTaskNotifyGive(s_task);
    return true;
}

void mcp2515_get_stats(bridge_bus_stats_t *out)
{
    *out = s_stats;
}
