See https://www.thingiverse.com/thing:7402116
The counter is a GPS-disciplined instrument with atomic accuracy, measuring frequency with sub-ppm precision across the 1 Hz to 40 MHz range. The instrument can be used to fine tune quartz oscillators.

Upon startup, the ESP32-P4 operates in counter mode: it opens a counter gate for one second and counts the incoming pulses. This count, which represents the frequency, is displayed in hertz on the TFT. The accuracy and granularity of this method are enhanced in the second mode, known as tuning mode. The rough count is known already, and the user can modify this up or down to set up the target frequency for the tuning.  In this mode, the time gap between the edge of the last incoming pulse and the 1 PPS pulse is measured using the highest possible frequency of 360 MHz. Since the resolution of this measurement is 2.8 ns, the main factor limiting accuracy is the GPS 1PPS jitter, which ranges from 20 ns to 50 ns. However, by averaging the count and compensating for jitter imperfections over a longer period, the error is effectively eliminated statistically, resulting in practically atomic accuracy.

 The project has been developed with extensive AI support from Google and DeepSeek. (The ESP32-P4 hardware manual, which is 3,707 pages long, is no longer intended for human reading.)

                    GPS-DISCIPLINED FREQUENCY COUNTER
                           ESP32-P4 IMPLEMENTATION


PROJECT OVERVIEW
================

This document describes the design and operation of a high-precision,
GPS-disciplined frequency counter built around the ESP32-P4 microcontroller.
The instrument measures frequencies from 1 Hz to approximately 40 MHz with
sub-PPM (up to 0.001 PPM) accuracy by using a GPS 1PPS (Pulse Per Second) signal as the
time reference.

Key Specifications:
  - Frequency Range: 1 Hz to ~40 MHz
  - Resolution: 1 Hz (direct count), sub-1 Hz with averaging
  - Accuracy: GPS-disciplined (depends on GPS module)
  - Time-base resolution: 2.78 ns (360 MHz CPU cycle counter)
  - Display: 1.8" TFT (170x320 or 320x170)
  - Modes: Counter (auto-range) and Crystal Tuner (PPM display)


HARDWARE ARCHITECTURE
====================

1. Main Controller: ESP32-P4
   - 360 MHz RISC-V dual-core CPU
   - 54-bit hardware timers
   - PCNT (Pulse Counter) peripheral for hardware pulse counting

2. GPS Module: NEO-6M or equivalent
   - Provides 1PPS reference signal
   - Provides NMEA serial data (time, satellites, lock status)

3. Input Signal Conditioning
   - Signal input on GPIO3
   - 3.3V logic levels required
   - External divider for frequencies above 40 MHz

4. User Interface
   - TFT Display (ST7789, 170x320 or 320x170)
   - 3 Tactile Buttons: MODE, UP, DOWN


PIN ASSIGNMENTS
===============

GPIO  | Function           | Description
------|--------------------|--------------------------------
GPIO2 | 1PPS Input         | GPS Pulse Per Second (rising edge)
GPIO3 | Frequency Input    | Signal to measure
GPIO4 | Button UP          | Increment target (+1 Hz in Tuner mode)
GPIO5 | Button DOWN        | Decrement target (-1 Hz in Tuner mode)
GPIO6 | Button MODE        | Toggle between Counter and Tuner modes
GPIO18| TFT SCLK           | SPI clock for display
GPIO19| TFT MOSI           | SPI data for display
GPIO20| TFT DC             | Data/Command for display
GPIO21| TFT CS             | Chip Select for display
GPIO22| TFT RST            | Reset for display
GPIO40| GPS RX (UART)      | NMEA data from GPS
GPIO41| GPS TX (UART)      | Configuration to GPS


OPERATING PRINCIPLES
====================

1. TIME-BASE: 1PPS FROM GPS
   -------------------------
   The GPS 1PPS signal provides an atomic-clock-referenced 1-second gate.
   Each rising edge is exactly 1 second apart (within GPS jitter of ~50 ns).


   ────┐     ┌────┐     ┌────┐     ┌────
   
       │     │    │     │    │     │
   
       └─────┘    └─────┘    └─────┘
   
       ^          ^          ^
       │          │          │
    START      STOP       START
    (1 sec)    (1 sec)    (1 sec)

3. PULSE COUNTING: PCNT + 64-BIT OVERFLOW
   ---------------------------------------
   The PCNT (Pulse Counter) peripheral counts pulses on GPIO3 in hardware.
   It is a 16-bit counter that overflows at ACCUMULATE_LIMIT (32,760).

   When the counter overflows, an interrupt adds ACCUMULATE_LIMIT to a
   64-bit accumulator (hf_overflow_count), allowing continuous counting
   without loss of pulses.

   Total Pulses = (hf_overflow_count × ACCUMULATE_LIMIT) + current_hf_raw

   Example at 11 MHz:
   ------------------
   ACCUMULATE_LIMIT     = 32,760
   Overflows per second ≈ 337
   hf_overflow_count    = 337 × 32,760 = 11,040,120
   current_hf_raw       = 18,880
   Total Pulses         = 11,059,000 ✓

4. TIME MEASUREMENT: 64-BIT CPU CYCLE COUNTER
   -------------------------------------------
   The CPU cycle counter (read_mcycle_64) provides 2.78 ns resolution
   by reading the 64-bit RISC-V cycle counter register.

   ─────────────────────────────────────────────────────────────────
   Timer Source           Resolution     Overflow Time
   ─────────────────────────────────────────────────────────────────
   esp_timer_get_time()   1 µs           584,000 years
   read_mcycle_64()       2.78 ns        1,624 years
   ─────────────────────────────────────────────────────────────────

5. THE MEASUREMENT SEQUENCE
   -------------------------
   a. 1PPS rising edge: START
      - Record CPU cycle count (start_cycles)
      - Record total pulse count (start_total)

   b. During the 1-second gate:
      - PCNT counts pulses in hardware
      - Overflow handler tracks 64-bit total

   c. Next 1PPS rising edge: STOP
      - Record CPU cycle count (stop_cycles)
      - Record total pulse count (stop_total)

   d. Calculate:
      - elapsed_cycles   = stop_cycles - start_cycles
      - elapsed_seconds  = elapsed_cycles / 360,000,000
      - pulses           = stop_total - start_total
      - frequency        = pulses / elapsed_seconds


DISPLAY MODES
=============

1. COUNTER MODE (Default)
   -----------------------
   Shows the measured frequency in Hz, kHz, or MHz with auto-ranging.
   The target frequency is automatically set to the rounded measured value.

   Display:
   ┌──────────────────────────┐
   │ Freq: 32768.000000 Hz    │
   │ Target: 32768 Hz         │
   │ Avg:   32768.000000 Hz   │
   │ Samples: 1234            │
   │ MODE: FREQUENCY COUNTER  │
   └──────────────────────────┘

2. CRYSTAL TUNER MODE
   -------------------
   Locks the target frequency for precise oscillator adjustment.
   Shows PPM deviation from target with color coding.
   UP/DN buttons adjust target in accelerating steps (1-100 Hz).

   Display:
   ┌──────────────────────────┐
   │ Freq: 32768.000000 Hz    │
   │ Target: 32768 Hz         │
   │ Avg:   32768.000000 Hz   │
   │ PPM: +0.0 PPM            │
   │ MANUAL CRYSTAL TUNER     │
   └──────────────────────────┘

3. PPM COLOR CODING
   -----------------
   ─────────────────────────────────────────────────────────────────
   PPM Range        Color        Meaning
   ─────────────────────────────────────────────────────────────────
   ±0.0 - 0.5      Green        Excellent
   ±0.5 - 5.0      Yellow       Good
   ±5.0 - 50.0     Orange       Marginal
   ±50.0 - 999.9   Red          Poor
   > 999.9         "OVF"        Overflow
   ─────────────────────────────────────────────────────────────────


BUTTON OPERATION
================

Button     Short Press          Long Press (Hold)
─────────────────────────────────────────────────────────────────────
MODE       Switch between       - (momentary action only)
           Counter/Tuner modes
─────────────────────────────────────────────────────────────────────
UP         +1 Hz                Accelerating: 1 → 5 → 10 → 50 → 100 Hz
─────────────────────────────────────────────────────────────────────
DOWN       -1 Hz                Accelerating: 1 → 5 → 10 → 50 → 100 Hz
─────────────────────────────────────────────────────────────────────


KEY CODE SECTIONS
=================

1. 64-Bit CPU Cycle Counter (Inline Assembly)
   -------------------------------------------
   static inline uint64_t read_mcycle_64(void) {
       uint32_t lo, hi, lo2;
       asm volatile (
           "rdcycleh %0\n"
           "rdcycle  %1\n"
           "rdcycleh %2\n"
           : "=r" (hi), "=r" (lo), "=r" (lo2)
           :
           : "memory"
       );
       if (hi != lo2) {
           asm volatile (
               "rdcycleh %0\n"
               "rdcycle  %1\n"
               : "=r" (hi), "=r" (lo)
               :
               : "memory"
           );
       }
       return ((uint64_t)hi << 32) | lo;
   }

2. 1PPS Interrupt (Hardware Gate)
   --------------------------------
   void IRAM_ATTR on1PPS_Interrupt() {
       uint64_t current_cycles = read_mcycle_64();
       int current_hf_raw = 0;
       pcnt_unit_get_count(hf_signal_unit, &current_hf_raw);
       uint64_t current_hf_total = (uint64_t)(hf_overflow_count + current_hf_raw);
       delta_hf_pulses = current_hf_total - last_hf_total_snapshot;
       delta_reference_cycles = current_cycles - last_gate_cycle;
       last_hf_total_snapshot = current_hf_total;
       last_gate_cycle = current_cycles;
       loop_measurement_ready = true;
   }

3. PCNT Overflow Handler
   -----------------------
   static bool IRAM_ATTR on_hf_overflow(pcnt_unit_handle_t unit,
                                         const pcnt_watch_event_data_t *edata,
                                         void *user_ctx) {
       if (edata->watch_point_value == ACCUMULATE_LIMIT) {
           hf_overflow_count += ACCUMULATE_LIMIT;
       }
       return false;
   }

4. Frequency Calculation
   -----------------------
   void run_comparison_math() {
       if (delta_reference_cycles == 0) return;
       double elapsed_seconds = (double)delta_reference_cycles / (double)CPU_FREQ;
       double exact_calculated_frequency = (double)delta_hf_pulses / elapsed_seconds;
       if (!tuning_mode_active) {
           user_tuned_target_baseline = round(exact_calculated_frequency);
       } else {
           current_ppm = ((exact_calculated_frequency - user_tuned_target_baseline)
                          / user_tuned_target_baseline) * 1000000.0;
       }
       // ... update display
   }


TROUBLESHOOTING
===============

1. No Display
   -----------
   - Check TFT connections (SCLK, MOSI, CS, DC, RST, BL)
   - Verify power (3.3V)
   - Check SPI.begin() call

2. No 1PPS Interrupt
   -------------------
   - Check GPS module power and antenna
   - Verify GPS lock status (LED flashing)
   - Check GPIO2 connection
   - Verify attachInterrupt() with RISING edge

3. Counter Shows Zero
   -------------------
   - Check signal input on GPIO3 (3.3V level)
   - Check signal frequency (counter works from 1 Hz to 40 MHz)
   - At 11.059 MHz, occasional zeros are expected (phase lock artifact)

4. Erratic Readings
   ------------------
   - Check GPS lock (satellite count)
   - Check signal quality (clean edges)
   - Check power supply stability

5. Button Not Responding
   -----------------------
   - Check pull-up resistors (INPUT_PULLUP)
   - Check debounce timing in ISR
   - Check button wiring


SPECIFICATIONS SUMMARY
======================

Parameter                Value
─────────────────────────────────────────────────────────────────
Microcontroller          ESP32-P4 (360 MHz RISC-V)
Timer Resolution        2.78 ns (CPU cycle counter)
Frequency Range         1 Hz to ~40 MHz
Measurement Gate        1 second (GPS 1PPS)
Display                 1.8" TFT (170x320 or 320x170)
Input Voltage           3.3V logic (5V tolerant with level shifter)
Power Supply            3.3V (USB or external)
Modes                   Counter, Crystal Tuner
Accuracy                GPS-disciplined (< 0.001 PPM with averaging)
GPS Module              NEO-6M, NEO-M8N, or MAX-M10S


    Author: ESP32-P4 GPS-Disciplined Frequency Counter Project
    Date: August 2026
    Version: 1.0
