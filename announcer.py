import array
import time

import board
from digitalio import DigitalInOut, Pull
from rp2pio import StateMachine

import an


BAUDRATE = 115_200
FIRST_GPIO = 0
GPIO_COUNT = an.GPIO_COUNT
IDLE_HIGH_MASK = (1 << GPIO_COUNT) - 1
FLOATING_SETTLE_SECONDS = 0.001


def board_gpio_names():
    for number in range(FIRST_GPIO, FIRST_GPIO + GPIO_COUNT):
        name = "GP{}".format(number)
        if hasattr(board, name):
            yield name, number - FIRST_GPIO


def append_uart_words(words, pin_index, data):
    pin_mask = 1 << pin_index
    selected_low = IDLE_HIGH_MASK & ~pin_mask

    for byte in data:
        words.append(selected_low)  # Start bit.
        for bit_number in range(8):
            if byte & (1 << bit_number):
                words.append(IDLE_HIGH_MASK)
            else:
                words.append(selected_low)
        words.append(IDLE_HIGH_MASK)  # Stop bit.


def detect_pin(pin):
    probe = DigitalInOut(pin)
    try:
        probe.switch_to_input(pull=Pull.UP)
        time.sleep(FLOATING_SETTLE_SECONDS)
        pulled_up = probe.value

        probe.switch_to_input(pull=Pull.DOWN)
        time.sleep(FLOATING_SETTLE_SECONDS)
        pulled_down = probe.value
    finally:
        probe.deinit()

    if pulled_up and not pulled_down:
        return "floating"
    if pulled_up and pulled_down:
        return "tied high"
    if not pulled_up and not pulled_down:
        return "tied low"
    return "externally driven or changing"


gpio_names = tuple(board_gpio_names())

if not gpio_names:
    raise RuntimeError("board has no GP<n> pins in the configured range")

floating_gpio_names = []
for name, pin_index in gpio_names:
    status = detect_pin(getattr(board, name))
    print("{}: {}".format(name, status))
    if status == "floating":
        floating_gpio_names.append((name, pin_index))

if not floating_gpio_names:
    raise RuntimeError("no floating GPIO pins in the configured range")

active_pin_mask = 0
transmit_words = array.array("I")
for name, pin_index in floating_gpio_names:
    active_pin_mask |= 1 << pin_index
    append_uart_words(
        transmit_words,
        pin_index,
        (name + "\r\n").encode("ascii"),
    )

first_pin = getattr(board, "GP{}".format(FIRST_GPIO))
uart = StateMachine(
    an.PROGRAM,
    frequency=BAUDRATE,
    first_out_pin=first_pin,
    out_pin_count=GPIO_COUNT,
    initial_out_pin_state=IDLE_HIGH_MASK,
    initial_out_pin_direction=active_pin_mask,
    auto_pull=True,
    pull_threshold=GPIO_COUNT,
    out_shift_right=True,
    **an.PIO_KWARGS,
)
uart.background_write(loop=transmit_words)

while True:
    time.sleep(1)
