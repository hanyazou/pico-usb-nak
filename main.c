/*
 * pico-usb-nak for Waveshare RP2040-Zero
 */
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/stdio_uart.h"
#include "pico/stdio_usb.h"
#include "hardware/pio.h"
#include "hardware/resets.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"
#include "hardware/structs/usb.h"
#include "hardware/regs/usb.h"
#include "ws2812.pio.h"

#ifndef STARTUP_WINDOW_MS
#define STARTUP_WINDOW_MS 6000
#endif

#ifndef WS2812_PIN
#ifdef PICO_DEFAULT_WS2812_PIN
#define WS2812_PIN PICO_DEFAULT_WS2812_PIN
#else
#define WS2812_PIN 16               /* WS2812 on RP2040-Zero is GP16 */
#endif
#endif

/* Set to 1 if red and green appear swapped (RGB order) */
#ifndef LED_ORDER_RGB
#define LED_ORDER_RGB 0
#endif

#define LED_LEVEL 24                /* keep it dim, the LED is very bright */

/* Mode is handed over across reboot in watchdog scratch[0] (scratch[4..7] are used by the SDK) */
#define MODE_MAGIC  0x55B0D000u
#define MODE_MASK   0x000000FFu

enum mode { MODE_NONE = 0, MODE_NORMAL = 1, MODE_UNRESP = 2 };

/* ---------------------------------------------------------------- LED */

static void led_init(void)
{
    uint offset = pio_add_program(pio0, &ws2812_program);
    ws2812_program_init(pio0, 0, offset, WS2812_PIN, 800000, false);
}

static void led_set(uint8_t r, uint8_t g, uint8_t b)
{
#if LED_ORDER_RGB
    uint32_t v = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
#else
    uint32_t v = ((uint32_t)g << 16) | ((uint32_t)r << 8) | b;
#endif
    pio_sm_put_blocking(pio0, 0, v << 8u);
}

/* ------------------------------------------------------- BOOT button */

/*
 * The BOOT button is wired to QSPI_SS (the flash chip select), so briefly
 * disable the CS output and read the pin. Flash cannot be accessed meanwhile,
 * so this runs from RAM. Same technique as pico-examples/picoboard/button
 * (RP2040 only).
 */
static bool __no_inline_not_in_flash_func(bootsel_raw)(void)
{
    const uint CS_PIN_INDEX = 1;
    uint32_t flags = save_and_disable_interrupts();

    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
    for (volatile int i = 0; i < 1000; ++i)
        ;
    bool pressed = !(sio_hw->gpio_hi_in & (1u << CS_PIN_INDEX));

    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_NORMAL << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
    restore_interrupts(flags);
    return pressed;
}

/* Returns true on a press. Debounces and waits for release. */
static bool bootsel_clicked(void)
{
    if (!bootsel_raw())
        return false;
    sleep_ms(30);
    if (!bootsel_raw())
        return false;
    while (bootsel_raw())
        sleep_ms(10);
    return true;
}

/* ------------------------------------------------------ mode handoff */

static enum mode take_saved_mode(void)
{
    uint32_t v = watchdog_hw->scratch[0];
    watchdog_hw->scratch[0] = 0;
    if ((v & ~MODE_MASK) != MODE_MAGIC)
        return MODE_NONE;
    return (enum mode)(v & MODE_MASK);
}

static void __attribute__((noreturn)) reboot_into(enum mode m)
{
    printf("rebooting into %s mode\n", m == MODE_UNRESP ? "UNRESP" : "NORMAL");
    stdio_flush();

    /* Make sure the host sees a detach before rebooting */
    hw_clear_bits(&usb_hw->sie_ctrl, USB_SIE_CTRL_PULLUP_EN_BITS);
    led_set(0, 0, 0);
    sleep_ms(500);

    watchdog_hw->scratch[0] = MODE_MAGIC | (uint32_t)m;
    watchdog_reboot(0, 0, 0);
    for (;;)
        tight_loop_contents();
}

/* ------------------------------------------------------- UNRESP mode */

static void usb_unresponsive_init(void)
{
    reset_block(RESETS_RESET_USBCTRL_BITS);
    unreset_block_wait(RESETS_RESET_USBCTRL_BITS);

    /* All buffer control words are 0 = no buffer is AVAILABLE -> always NAK */
    memset((void *)usb_dpram, 0, sizeof(*usb_dpram));

    usb_hw->muxing = USB_USB_MUXING_TO_PHY_BITS | USB_USB_MUXING_SOFTCON_BITS;
    usb_hw->pwr = USB_USB_PWR_VBUS_DETECT_BITS |
                  USB_USB_PWR_VBUS_DETECT_OVERRIDE_EN_BITS;
    usb_hw->main_ctrl = USB_MAIN_CTRL_CONTROLLER_EN_BITS;
    usb_hw->inte = 0;
    usb_hw->sie_ctrl = USB_SIE_CTRL_PULLUP_EN_BITS;
}

static void __attribute__((noreturn)) run_unresponsive(void)
{
    usb_unresponsive_init();
    led_set(LED_LEVEL, 0, 0);
    printf("UNRESP mode: pull-up on, EP0 NAKs forever\n");

    for (;;) {
        if (bootsel_clicked())
            reboot_into(MODE_NORMAL);
        sleep_ms(20);
    }
}

/* ------------------------------------------------------- NORMAL mode */

static void print_usage(void)
{
    printf(
        "\n"
        "=== pico-usb-nak (RP2040-Zero) ===\n"
        "This board emulates an unresponsive USB device for host tests.\n"
        "\n"
        "Current mode: NORMAL (you are reading this over USB CDC)\n"
        "\n"
        "How to use:\n"
        "  * Press BOOT now      -> reboot into UNRESP mode (LED red).\n"
        "                           The host should report\n"
        "                           'unable to enumerate USB device'.\n"
        "  * In UNRESP, BOOT     -> back to NORMAL mode (LED green).\n"
        "  * After plug-in the LED blinks blue for %d ms with USB detached.\n"
        "    Press BOOT during that time to start directly in UNRESP mode.\n"
        "\n"
        "  Do NOT hold BOOT while plugging in: that starts the ROM\n"
        "  bootloader (UF2 drive) instead of this firmware.\n"
        "\n"
        "Reflash: 'picotool load -f pico-usb-nak.uf2' works in NORMAL mode;\n"
        "         otherwise hold BOOT, press RESET, release BOOT.\n"
        "\n",
        STARTUP_WINDOW_MS);
}

static void __attribute__((noreturn)) run_normal(void)
{
    stdio_usb_init();
    led_set(0, LED_LEVEL, 0);
    printf("NORMAL mode\n");

    bool was_connected = false;
    for (;;) {
        bool connected = stdio_usb_connected();   /* terminal asserted DTR? */
        if (connected && !was_connected) {
            sleep_ms(1000);                       /* give the terminal time to get ready */
            print_usage();
        }
        was_connected = connected;

        if (bootsel_clicked())
            reboot_into(MODE_UNRESP);
        sleep_ms(20);
    }
}

/* ------------------------------------------------------ startup window */

static enum mode startup_window(void)
{
#ifdef MODE_SWITCH_PIN
    gpio_init(MODE_SWITCH_PIN);
    gpio_set_dir(MODE_SWITCH_PIN, GPIO_IN);
    gpio_pull_up(MODE_SWITCH_PIN);
    sleep_ms(1);
    if (!gpio_get(MODE_SWITCH_PIN))
        return MODE_UNRESP;
#endif
    absolute_time_t end = make_timeout_time_ms(STARTUP_WINDOW_MS);
    uint32_t t = 0;
    while (!time_reached(end)) {
        led_set(0, 0, ((t / 200) & 1) ? 0 : LED_LEVEL);
        if (bootsel_clicked())
            return MODE_UNRESP;
        sleep_ms(20);
        t += 20;
    }
    return MODE_NORMAL;
}

int main(void)
{
    stdio_uart_init();      /* log always goes to UART0 (GP0/GP1) as well */
    led_init();

    enum mode m = take_saved_mode();
    if (m == MODE_NONE)
        m = startup_window();

    if (m == MODE_UNRESP)
        run_unresponsive();
    run_normal();
}
