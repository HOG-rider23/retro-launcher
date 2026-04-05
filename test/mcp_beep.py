#!/usr/bin/env python3
import time
import smbus2

# === CONFIGURATION ===
I2C_BUS = 11         # ← Change if needed (run "ls /dev/i2c*" and "i2cdetect -y X" to find your bus)
MCP_ADDR = 0x27      # Default address (A0=A1=A2=ground)
BUZZER_PIN = 0       # GPA0 = first pin on port A (physical pin 21 on MCP23017)

# Open I²C bus
bus = smbus2.SMBus(I2C_BUS)

def mcp_write(reg, value):
    bus.write_byte_data(MCP_ADDR, reg, value)

# Initialise MCP23017
mcp_write(0x00, 0x00)   # IODIRA = all outputs on Port A
mcp_write(0x01, 0x00)   # IODIRB = all outputs on Port B (optional)
mcp_write(0x14, 0x00)   # Turn all outputs off initially (GPIOA)

def play_tone(freq_hz=800, duration_sec=0.2):
    if freq_hz < 50:
        freq_hz = 50
    period = 1.0 / freq_hz
    half_period = period / 2
    end_time = time.time() + duration_sec

    while time.time() < end_time:
        # Turn buzzer pin ON
        mcp_write(0x14, 1 << BUZZER_PIN)   # GPIOA
        time.sleep(half_period)
        # Turn buzzer pin OFF
        mcp_write(0x14, 0x00)
        time.sleep(half_period)

# === TEST ===
print("Testing buzzer on GPA0...")
play_tone(800, 0.2)   # low beep
time.sleep(0.05)
play_tone(1200, 0.2)  # higher beep
time.sleep(0.05)
play_tone(2000, 0.3)  # shoot / alert sound