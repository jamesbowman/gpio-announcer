# GPIO Announcer

GPIO Announcer is a tiny CircuitPython program that transmits each
pin's name, such as 'GP15\r\n', over that pin as 115200-baud 8N1
serial.

It first scans GPIOS 0-22 to find floating pins, avoiding driving
pins that are wired as inputs.
