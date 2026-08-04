#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/pio_instructions.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

#define BAUDRATE 115200u
#define FIRST_GPIO 0u
#define GPIO_COUNT 23u
#define UART_BITS_PER_BYTE 10u
#define MAX_MESSAGE_BYTES 6u       // "GP22\r\n"
#define MAX_TRANSMIT_WORDS (GPIO_COUNT * MAX_MESSAGE_BYTES * UART_BITS_PER_BYTE)
#define FLOATING_SETTLE_US 1000u
#define IDLE_HIGH_MASK ((1u << GPIO_COUNT) - 1u)

_Static_assert(GPIO_COUNT < 32u, "GPIO masks must fit in one DMA word");
_Static_assert(FIRST_GPIO + GPIO_COUNT <= NUM_BANK0_GPIOS,
               "Configured GPIO range is not available on this target");

typedef enum {
    PIN_FLOATING,
    PIN_TIED_HIGH,
    PIN_TIED_LOW,
    PIN_DRIVEN_OR_CHANGING,
} pin_status_t;

static uint32_t transmit_words[MAX_TRANSMIT_WORDS];
static size_t transmit_word_count;
static uintptr_t dma_restart_address;

// pull block; then one out instruction per 115200-baud UART bit.
static const uint16_t announcer_instructions[] = {
    pio_instr_bits_pull | (1u << 5u),
    pio_instr_bits_out | GPIO_COUNT,
};

static const pio_program_t announcer_program = {
    .instructions = announcer_instructions,
    .length = 2,
    .origin = -1,
};

static const char *pin_status_name(pin_status_t status) {
    switch (status) {
        case PIN_FLOATING:
            return "floating";
        case PIN_TIED_HIGH:
            return "tied high";
        case PIN_TIED_LOW:
            return "tied low";
        default:
            return "externally driven or changing";
    }
}

static pin_status_t detect_pin(uint pin) {
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);

    gpio_set_pulls(pin, true, false);
    sleep_us(FLOATING_SETTLE_US);
    bool pulled_up = gpio_get(pin);

    gpio_set_pulls(pin, false, true);
    sleep_us(FLOATING_SETTLE_US);
    bool pulled_down = gpio_get(pin);

    gpio_deinit(pin);

    if (pulled_up && !pulled_down) {
        return PIN_FLOATING;
    }
    if (pulled_up && pulled_down) {
        return PIN_TIED_HIGH;
    }
    if (!pulled_up && !pulled_down) {
        return PIN_TIED_LOW;
    }
    return PIN_DRIVEN_OR_CHANGING;
}

static void append_word(uint32_t word) {
    if (transmit_word_count >= MAX_TRANSMIT_WORDS) {
        panic("transmit_words overflow");
    }
    transmit_words[transmit_word_count++] = word;
}

static void append_uart_bytes(uint pin_index, const uint8_t *data, size_t length) {
    uint32_t pin_mask = 1u << pin_index;
    uint32_t selected_low = IDLE_HIGH_MASK & ~pin_mask;

    for (size_t byte_index = 0; byte_index < length; ++byte_index) {
        uint8_t byte = data[byte_index];

        append_word(selected_low);  // Start bit.
        for (uint bit = 0; bit < 8u; ++bit) {
            append_word((byte & (1u << bit)) ? IDLE_HIGH_MASK : selected_low);
        }
        append_word(IDLE_HIGH_MASK);  // Stop bit.
    }
}

static uint32_t build_transmit_words(void) {
    uint32_t active_pin_mask = 0;

    for (uint pin = FIRST_GPIO; pin < FIRST_GPIO + GPIO_COUNT; ++pin) {
        pin_status_t status = detect_pin(pin);
        printf("GP%u: %s\n", pin, pin_status_name(status));

        if (status == PIN_FLOATING) {
            char message[MAX_MESSAGE_BYTES + 1u];
            int length = snprintf(message, sizeof(message), "GP%u\r\n", pin);
            if (length < 0 || (size_t)length > MAX_MESSAGE_BYTES) {
                panic("GPIO name does not fit message buffer");
            }

            uint pin_index = pin - FIRST_GPIO;
            active_pin_mask |= 1u << pin_index;
            append_uart_bytes(pin_index, (const uint8_t *)message, (size_t)length);
        }
    }

    return active_pin_mask;
}

static void start_pio(PIO *pio_out, uint *sm_out, uint32_t active_pin_mask) {
    PIO pio;
    uint sm;
    uint offset;

    bool claimed = pio_claim_free_sm_and_add_program_for_gpio_range(
        &announcer_program, &pio, &sm, &offset, FIRST_GPIO, GPIO_COUNT, true);
    if (!claimed) {
        panic("no PIO state machine/program space available");
    }

    for (uint pin = FIRST_GPIO; pin < FIRST_GPIO + GPIO_COUNT; ++pin) {
        pio_gpio_init(pio, pin);
    }

    pio_sm_config config = pio_get_default_sm_config();
    sm_config_set_wrap(&config, offset + 1u, offset + 1u);
    sm_config_set_out_pins(&config, FIRST_GPIO, GPIO_COUNT);
    sm_config_set_out_shift(&config, true, true, GPIO_COUNT);
    sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv(&config, (float)clock_get_hz(clk_sys) / (float)BAUDRATE);

    pio_sm_init(pio, sm, offset, &config);
    pio_sm_set_pins_with_mask(pio, sm, IDLE_HIGH_MASK, IDLE_HIGH_MASK);
    pio_sm_set_pindirs_with_mask(pio, sm, active_pin_mask, IDLE_HIGH_MASK);

    *pio_out = pio;
    *sm_out = sm;
}

static void start_looping_dma(PIO pio, uint sm) {
    int data_channel = dma_claim_unused_channel(true);
    int control_channel = dma_claim_unused_channel(true);

    dma_channel_config data_config = dma_channel_get_default_config(data_channel);
    channel_config_set_transfer_data_size(&data_config, DMA_SIZE_32);
    channel_config_set_read_increment(&data_config, true);
    channel_config_set_write_increment(&data_config, false);
    channel_config_set_dreq(&data_config, pio_get_dreq(pio, sm, true));
    channel_config_set_chain_to(&data_config, control_channel);
    dma_channel_configure(
        data_channel,
        &data_config,
        &pio->txf[sm],
        transmit_words,
        transmit_word_count,
        false);

    // Each completion writes the original buffer address to the data channel's
    // read-address trigger alias. Its transfer count reloads automatically.
    dma_restart_address = (uintptr_t)transmit_words;
    dma_channel_config control_config = dma_channel_get_default_config(control_channel);
    channel_config_set_transfer_data_size(&control_config, DMA_SIZE_32);
    channel_config_set_read_increment(&control_config, false);
    channel_config_set_write_increment(&control_config, false);
    dma_channel_configure(
        control_channel,
        &control_config,
        &dma_channel_hw_addr(data_channel)->al3_read_addr_trig,
        &dma_restart_address,
        1,
        false);

    dma_start_channel_mask(1u << data_channel);
    while (pio_sm_is_tx_fifo_empty(pio, sm)) {
        tight_loop_contents();
    }
    pio_sm_set_enabled(pio, sm, true);
}

int main(void) {
    stdio_init_all();

    uint32_t active_pin_mask = build_transmit_words();
    if (active_pin_mask == 0u) {
        panic("no floating GPIO pins in the configured range");
    }

    PIO pio;
    uint sm;
    start_pio(&pio, &sm, active_pin_mask);
    start_looping_dma(pio, sm);

    printf("announcing %u words across %u GPIOs at %u baud\n",
           (unsigned)transmit_word_count,
           (unsigned)__builtin_popcount(active_pin_mask),
           BAUDRATE);

    while (true) {
        __wfi();
    }
}
