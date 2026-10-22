// Эмуляция протокола CP2112 (Silicon Labs AN495) поверх аппаратного I2C RP2040.
// Все настройки, которые хост "прошивает" в чип (строки, USB config, lock byte),
// хранятся только в RAM и сбрасываются при переподключении.

#include "cp2112.h"
#include "config.h"
#include "smbus.h"

#include <string.h>

#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "hardware/watchdog.h"
#include "tusb.h"

#define STATUS0_IDLE        0x00
#define STATUS0_BUSY        0x01
#define STATUS0_COMPLETE    0x02
#define STATUS0_ERROR       0x03

#define STATUS1_TIMEOUT_NACK     0x00
#define STATUS1_TIMEOUT_BUS      0x01
#define STATUS1_ARBITRATION_LOST 0x02
#define STATUS1_READ_INCOMPLETE  0x03
#define STATUS1_WRITE_INCOMPLETE 0x04
#define STATUS1_SUCCESS          0x05

#define MAX_READ_LEN        512
#define MAX_TARGET_LEN      16
#define MAX_WRITE_LEN       61
#define READ_CHUNK          61

// ---------------------------------------------------------------------------
// Состояние

typedef struct {
    uint32_t clock_hz;
    uint8_t  device_address;
    uint8_t  auto_send_read;
    uint16_t write_timeout_ms;
    uint16_t read_timeout_ms;
    uint8_t  scl_low_timeout;
    uint16_t retry_time;
} smbus_config_t;

static smbus_config_t cfg;

typedef enum { CMD_NONE, CMD_READ, CMD_WRITE_READ, CMD_WRITE } cmd_type_t;

static struct {
    cmd_type_t type;
    uint8_t    addr;            // 7-битный
    uint16_t   read_len;
    uint8_t    target_len;
    uint8_t    target[MAX_TARGET_LEN];
    uint8_t    write_len;
    uint8_t    write_data[MAX_WRITE_LEN];
} pending;

static struct {
    uint8_t  status0;
    uint8_t  status1;
    uint16_t retries;
    uint16_t bytes_read;
} xfer;

static uint8_t  rx_buf[MAX_READ_LEN];
static uint16_t rx_len;         // сколько прочитано с шины
static uint16_t rx_sent;        // сколько уже отдано хосту

// Очередь входящих (IN) репортов: [0] = Report ID, далее данные
#define IN_QUEUE_LEN 12
static uint8_t  in_queue[IN_QUEUE_LEN][64];
static uint8_t  in_queue_size[IN_QUEUE_LEN];
static uint8_t  in_head, in_tail, in_count;

static uint8_t gpio_config[LEN_GPIO_CONFIG];
static uint8_t gpio_latch = 0xFF;
static uint8_t lock_byte = 0xFF;
static uint8_t usb_config[LEN_USB_CONFIG];

static uint8_t str_manufacturer[LEN_STRING];
static uint8_t str_product[LEN_STRING];
static uint8_t str_serial[LEN_STRING];

static char ascii_manufacturer[31];
static char ascii_product[31];
static char ascii_serial[31];

static bool     reset_requested;
static uint32_t reset_at_ms;

// ---------------------------------------------------------------------------
// Вспомогательные функции

static void put_be16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v & 0xFF; }
static uint16_t get_be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

// Строковый репорт: [длина дескриптора, 0x03, UTF-16LE...]
static void make_string_report(uint8_t *dst, const char *s) {
    memset(dst, 0, LEN_STRING);
    size_t n = strlen(s);
    if (n > 30) n = 30;
    dst[0] = (uint8_t)(2 + 2 * n);
    dst[1] = 0x03;
    for (size_t i = 0; i < n; i++) dst[2 + 2 * i] = (uint8_t)s[i];
}

static void string_report_to_ascii(char *dst, const uint8_t *src) {
    size_t n = 0;
    if (src[1] == 0x03 && src[0] >= 2) {
        n = (src[0] - 2) / 2;
        if (n > 30) n = 30;
    }
    for (size_t i = 0; i < n; i++) dst[i] = (char)src[2 + 2 * i];
    dst[n] = '\0';
}

static void queue_in(uint8_t id, const uint8_t *data, uint8_t len, uint8_t report_len) {
    if (in_count == IN_QUEUE_LEN) return;   // хост не забирает данные — теряем
    uint8_t *slot = in_queue[in_tail];
    memset(slot, 0, 64);
    slot[0] = id;
    memcpy(slot + 1, data, len);
    in_queue_size[in_tail] = report_len;
    in_tail = (in_tail + 1) % IN_QUEUE_LEN;
    in_count++;
}

static void flush_in_queue(void) {
    in_head = in_tail = in_count = 0;
}

static void send_read_response(uint8_t status, const uint8_t *data, uint8_t len) {
    uint8_t r[LEN_READ_RESPONSE] = {0};
    r[0] = status;
    r[1] = len;
    memcpy(r + 2, data, len);
    queue_in(RPT_READ_RESPONSE, r, sizeof(r), LEN_READ_RESPONSE);
}

// Отдаёт хосту до limit байт из rx_buf кусками по 61 байту
static void push_rx_data(uint16_t limit) {
    uint16_t left = rx_len - rx_sent;
    if (limit > left) limit = left;

    if (limit == 0) {
        send_read_response(xfer.status0, NULL, 0);
        return;
    }

    while (limit > 0) {
        uint8_t n = limit > READ_CHUNK ? READ_CHUNK : (uint8_t)limit;
        rx_sent += n;
        limit -= n;
        uint8_t st = (rx_sent == rx_len) ? STATUS0_COMPLETE : STATUS0_BUSY;
        send_read_response(st, rx_buf + rx_sent - n, n);
    }
}

static void set_defaults(void) {
    cfg = (smbus_config_t){
        .clock_hz = SMBUS_DEFAULT_HZ,
        .device_address = 0x02,
        .auto_send_read = 0,
        .write_timeout_ms = 0,
        .read_timeout_ms = 0,
        .scl_low_timeout = 0,
        .retry_time = 0,
    };

    memset(gpio_config, 0, sizeof(gpio_config));
    gpio_latch = 0xFF;
    lock_byte = 0xFF;

    usb_config[0] = CP2112_VID & 0xFF;
    usb_config[1] = CP2112_VID >> 8;
    usb_config[2] = CP2112_PID & 0xFF;
    usb_config[3] = CP2112_PID >> 8;
    usb_config[4] = 50;     // 100 мА в единицах 2 мА
    usb_config[5] = 0x00;   // питание от шины
    usb_config[6] = 1;      // release major
    usb_config[7] = 0;      // release minor
    usb_config[8] = 0;      // маска (при чтении 0)

    pico_unique_board_id_t uid;
    pico_get_unique_board_id(&uid);
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < 4; i++) {
        ascii_serial[i * 2]     = hex[uid.id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES - 4 + i] >> 4];
        ascii_serial[i * 2 + 1] = hex[uid.id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES - 4 + i] & 0xF];
    }
    ascii_serial[8] = '\0';

    strcpy(ascii_manufacturer, "Silicon Laboratories");
    strcpy(ascii_product, "CP2112 HID USB-to-SMBus Bridge");

    make_string_report(str_manufacturer, ascii_manufacturer);
    make_string_report(str_product, ascii_product);
    make_string_report(str_serial, ascii_serial);
}

// ---------------------------------------------------------------------------
// Выполнение транзакций

static void finish_transfer(smbus_result_t res, uint16_t retries) {
    xfer.retries = retries;
    switch (res) {
    case SMBUS_OK:
        xfer.status0 = STATUS0_COMPLETE;
        xfer.status1 = STATUS1_SUCCESS;
        break;
    case SMBUS_ADDR_NACK:
        xfer.status0 = STATUS0_ERROR;
        xfer.status1 = STATUS1_TIMEOUT_NACK;
        break;
    case SMBUS_DATA_NACK:
        xfer.status0 = STATUS0_ERROR;
        xfer.status1 = pending.type == CMD_WRITE ? STATUS1_WRITE_INCOMPLETE : STATUS1_READ_INCOMPLETE;
        break;
    case SMBUS_BUS_TIMEOUT:
        xfer.status0 = STATUS0_ERROR;
        xfer.status1 = STATUS1_TIMEOUT_BUS;
        break;
    }
}

static void run_pending(void) {
    if (pending.type == CMD_NONE) return;

    uint16_t max_retries = cfg.retry_time;
    if (max_retries > SMBUS_MAX_RETRIES) max_retries = SMBUS_MAX_RETRIES;

    smbus_result_t res = SMBUS_OK;
    uint16_t attempt = 0;
    for (;;) {
        switch (pending.type) {
        case CMD_WRITE:
            res = smbus_write(pending.addr, pending.write_data, pending.write_len, cfg.write_timeout_ms);
            break;
        case CMD_READ:
            res = smbus_read(pending.addr, rx_buf, pending.read_len, cfg.read_timeout_ms);
            break;
        case CMD_WRITE_READ:
            res = smbus_write_read(pending.addr, pending.target, pending.target_len,
                                   rx_buf, pending.read_len, cfg.read_timeout_ms);
            break;
        default:
            break;
        }
        if (res != SMBUS_ADDR_NACK || attempt >= max_retries) break;
        attempt++;
    }

    bool is_read = pending.type != CMD_WRITE;
    rx_sent = 0;
    rx_len = (is_read && res == SMBUS_OK) ? pending.read_len : 0;
    xfer.bytes_read = rx_len;

    finish_transfer(res, attempt);

    if (is_read && cfg.auto_send_read) {
        if (res == SMBUS_OK) push_rx_data(rx_len);
        else send_read_response(STATUS0_ERROR, NULL, 0);
    }

    pending.type = CMD_NONE;
}

// ---------------------------------------------------------------------------
// Обработчики репортов

uint16_t cp2112_get_feature(uint8_t id, uint8_t *buf, uint16_t maxlen) {
    uint8_t tmp[64];
    uint16_t len = 0;

    switch (id) {
    case RPT_GPIO_CONFIG:
        memcpy(tmp, gpio_config, LEN_GPIO_CONFIG);
        len = LEN_GPIO_CONFIG;
        break;
    case RPT_GPIO_GET:
        tmp[0] = gpio_latch;
        len = LEN_GPIO_GET;
        break;
    case RPT_VERSION:
        tmp[0] = CP2112_PART_NUMBER;
        tmp[1] = CP2112_DEVICE_VER;
        len = LEN_VERSION;
        break;
    case RPT_SMBUS_CONFIG:
        tmp[0] = cfg.clock_hz >> 24;
        tmp[1] = cfg.clock_hz >> 16;
        tmp[2] = cfg.clock_hz >> 8;
        tmp[3] = cfg.clock_hz;
        tmp[4] = cfg.device_address;
        tmp[5] = cfg.auto_send_read;
        put_be16(tmp + 6, cfg.write_timeout_ms);
        put_be16(tmp + 8, cfg.read_timeout_ms);
        tmp[10] = cfg.scl_low_timeout;
        put_be16(tmp + 11, cfg.retry_time);
        len = LEN_SMBUS_CONFIG;
        break;
    case RPT_LOCK_BYTE:
        tmp[0] = lock_byte;
        len = LEN_LOCK_BYTE;
        break;
    case RPT_USB_CONFIG:
        memcpy(tmp, usb_config, LEN_USB_CONFIG);
        len = LEN_USB_CONFIG;
        break;
    case RPT_MANUFACTURER_STRING:
        memcpy(tmp, str_manufacturer, LEN_STRING);
        len = LEN_STRING;
        break;
    case RPT_PRODUCT_STRING:
        memcpy(tmp, str_product, LEN_STRING);
        len = LEN_STRING;
        break;
    case RPT_SERIAL_STRING:
        memcpy(tmp, str_serial, LEN_STRING);
        len = LEN_STRING;
        break;
    default:
        return 0;
    }

    if (len > maxlen) len = maxlen;
    memcpy(buf, tmp, len);
    return len;
}

void cp2112_set_feature(uint8_t id, const uint8_t *buf, uint16_t len) {
    switch (id) {
    case RPT_RESET:
        if (len >= 1 && buf[0] == 0x01) {
            reset_requested = true;
            reset_at_ms = to_ms_since_boot(get_absolute_time()) + 50;
        }
        break;
    case RPT_GPIO_CONFIG:
        if (len >= LEN_GPIO_CONFIG) memcpy(gpio_config, buf, LEN_GPIO_CONFIG);
        break;
    case RPT_GPIO_SET:
        if (len >= LEN_GPIO_SET) gpio_latch = (gpio_latch & ~buf[1]) | (buf[0] & buf[1]);
        break;
    case RPT_SMBUS_CONFIG:
        if (len < LEN_SMBUS_CONFIG) break;
        cfg.clock_hz = (uint32_t)buf[0] << 24 | (uint32_t)buf[1] << 16 | (uint32_t)buf[2] << 8 | buf[3];
        cfg.device_address = buf[4];
        cfg.auto_send_read = buf[5] ? 1 : 0;
        cfg.write_timeout_ms = get_be16(buf + 6);
        cfg.read_timeout_ms = get_be16(buf + 8);
        cfg.scl_low_timeout = buf[10];
        cfg.retry_time = get_be16(buf + 11);
        smbus_set_clock(cfg.clock_hz);
        break;
    case RPT_LOCK_BYTE:
        if (len >= 1) lock_byte &= buf[0];
        break;
    case RPT_USB_CONFIG:
        // Принимаем, но VID/PID не меняем: иначе Be2Works перестанет видеть адаптер
        if (len >= LEN_USB_CONFIG) {
            uint8_t mask = buf[8];
            if (mask & 0x04) usb_config[4] = buf[4];
            if (mask & 0x08) usb_config[5] = buf[5];
            if (mask & 0x10) { usb_config[6] = buf[6]; usb_config[7] = buf[7]; }
        }
        break;
    case RPT_MANUFACTURER_STRING:
        if (len >= 2) {
            memset(str_manufacturer, 0, LEN_STRING);
            memcpy(str_manufacturer, buf, len > LEN_STRING ? LEN_STRING : len);
            string_report_to_ascii(ascii_manufacturer, str_manufacturer);
        }
        break;
    case RPT_PRODUCT_STRING:
        if (len >= 2) {
            memset(str_product, 0, LEN_STRING);
            memcpy(str_product, buf, len > LEN_STRING ? LEN_STRING : len);
            string_report_to_ascii(ascii_product, str_product);
        }
        break;
    case RPT_SERIAL_STRING:
        if (len >= 2) {
            memset(str_serial, 0, LEN_STRING);
            memcpy(str_serial, buf, len > LEN_STRING ? LEN_STRING : len);
            string_report_to_ascii(ascii_serial, str_serial);
        }
        break;
    default:
        break;
    }
}

static void start_transfer(cmd_type_t type) {
    pending.type = type;
    xfer.status0 = STATUS0_BUSY;
    xfer.status1 = 0;
    xfer.retries = 0;
    xfer.bytes_read = 0;
    rx_len = rx_sent = 0;
}

void cp2112_on_output(uint8_t id, const uint8_t *buf, uint16_t len) {
    switch (id) {
    case RPT_READ_REQUEST: {
        if (len < LEN_READ_REQUEST) break;
        uint16_t n = get_be16(buf + 1);
        if (n == 0 || n > MAX_READ_LEN) break;
        pending.addr = buf[0] >> 1;
        pending.read_len = n;
        start_transfer(CMD_READ);
        break;
    }
    case RPT_WRITE_READ_REQUEST: {
        if (len < 4) break;
        uint16_t n = get_be16(buf + 1);
        uint8_t tl = buf[3];
        if (n == 0 || n > MAX_READ_LEN || tl == 0 || tl > MAX_TARGET_LEN || len < 4u + tl) break;
        pending.addr = buf[0] >> 1;
        pending.read_len = n;
        pending.target_len = tl;
        memcpy(pending.target, buf + 4, tl);
        start_transfer(CMD_WRITE_READ);
        break;
    }
    case RPT_WRITE: {
        if (len < 2) break;
        uint8_t n = buf[1];
        if (n == 0 || n > MAX_WRITE_LEN || len < 2u + n) break;
        pending.addr = buf[0] >> 1;
        pending.write_len = n;
        memcpy(pending.write_data, buf + 2, n);
        start_transfer(CMD_WRITE);
        break;
    }
    case RPT_READ_FORCE_SEND:
        if (len < LEN_READ_FORCE_SEND) break;
        push_rx_data(get_be16(buf));
        break;
    case RPT_XFER_STATUS_REQUEST: {
        uint8_t r[LEN_XFER_STATUS_RESP];
        r[0] = xfer.status0;
        r[1] = xfer.status1;
        put_be16(r + 2, xfer.retries);
        put_be16(r + 4, xfer.bytes_read);
        queue_in(RPT_XFER_STATUS_RESPONSE, r, sizeof(r), LEN_XFER_STATUS_RESP);
        break;
    }
    case RPT_CANCEL_TRANSFER:
        pending.type = CMD_NONE;
        rx_len = rx_sent = 0;
        xfer.status0 = STATUS0_IDLE;
        xfer.status1 = 0;
        flush_in_queue();
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------

void cp2112_init(void) {
    set_defaults();
    smbus_init(cfg.clock_hz);
}

void cp2112_task(void) {
    run_pending();

    if (in_count && tud_hid_ready()) {
        uint8_t *slot = in_queue[in_head];
        if (tud_hid_report(slot[0], slot + 1, in_queue_size[in_head])) {
            in_head = (in_head + 1) % IN_QUEUE_LEN;
            in_count--;
        }
    }

    if (reset_requested && to_ms_since_boot(get_absolute_time()) >= reset_at_ms) {
        tud_disconnect();
        sleep_ms(20);
        watchdog_reboot(0, 0, 0);
        for (;;) tight_loop_contents();
    }
}

const char *cp2112_usb_string(uint8_t index) {
    switch (index) {
    case 1: return ascii_manufacturer;
    case 2: return ascii_product;
    case 3: return ascii_serial;
    default: return NULL;
    }
}
