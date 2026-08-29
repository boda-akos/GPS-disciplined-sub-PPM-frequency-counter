// ============================================================
// ESP32-P4 STANDALONE TUNER - 64-bit CPU CYCLE COUNTER
// Using inline assembly for true 2.78ns resolution
// ============================================================

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include "driver/pulse_cnt.h"

// Pin Configuration Maps
#define TFT_MOSI        19
#define TFT_SCLK        18
#define TFT_CS          21
#define TFT_DC          20
#define TFT_RST         22
#define TFT_BL          33

#define PIN_1PPS_GATE   2  // Master 1Hz Pulse Input
#define PIN_HF_INPUT    3  // High-Frequency Signal to measure
#define PIN_BTN_UP      4  // Tact Switch: Increment Target Baseline (+1 Hz)
#define PIN_BTN_DOWN    5  // Tact Switch: Decrement Target Baseline (-1 Hz)
#define PIN_BTN_MODE    6  // Momentary Tact Switch: Cycle Modes & Show Guide Info

#define TFT_WIDTH   170
#define TFT_HEIGHT  320

Adafruit_ST7789 tft = Adafruit_ST7789(&SPI, TFT_CS, TFT_DC, TFT_RST);

// Counter Handles
pcnt_unit_handle_t hf_signal_unit = NULL;
pcnt_channel_handle_t hf_signal_chan = NULL;

// Variables tracking multi-bit hardware overflows continuously
volatile int64_t hf_overflow_count = 0;
const int ACCUMULATE_LIMIT = 32760; 

// Historical Tracking Values
volatile uint64_t last_hf_total_snapshot = 0;
volatile uint64_t last_gate_cycle = 0;  // 64-bit CPU cycle counter

// Atomic snapshot containers passed from ISR safely to Main Loop
volatile bool loop_measurement_ready = false;
volatile uint64_t delta_hf_pulses = 0;
volatile uint64_t delta_reference_cycles = 0;

// ============================================================
// STANDALONE USER VARIABLES
// ============================================================
// Button repeat control
unsigned long last_up_press_time = 0;
unsigned long last_down_press_time = 0;
uint32_t up_repeat_count = 0;
uint32_t down_repeat_count = 0;
bool up_pressed_continuous = false;
bool down_pressed_continuous = false;
volatile double user_tuned_target_baseline = 10000.0; 
volatile bool tuning_mode_active = false;              
volatile uint32_t sample_index = 0;

// Interrupt Action Flags
volatile bool flag_mode_pressed = false;
volatile bool flag_up_pressed = false;
volatile bool flag_down_pressed = false;

// Menu Display control sync flags
bool force_complete_redraw_after_menu = true;
void displayModePropertiesGuide(bool mode_target_state);
// CPU frequency
#define CPU_FREQ 360000000ULL

// ============================================================
// 64-BIT CPU CYCLE COUNTER WITH INLINE ASSEMBLY
// Returns the full 64-bit value of the mcycle CSR
// ============================================================

static inline uint64_t read_mcycle_64(void) {
    uint32_t lo, hi, lo2;
    
    // Read the 64-bit mcycle register using inline assembly
    // This works on any RISC-V core
    asm volatile (
        "rdcycleh %0\n"
        "rdcycle  %1\n"
        "rdcycleh %2\n"
        : "=r" (hi), "=r" (lo), "=r" (lo2)
        :
        : "memory"
    );
    
    // If the high value changed during read (32-bit overflow), read again
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

// Alternative: Single read (faster, but less safe)
static inline uint64_t read_mcycle_fast(void) {
    uint32_t lo, hi;
    asm volatile (
        "rdcycleh %0\n"
        "rdcycle  %1\n"
        : "=r" (hi), "=r" (lo)
        :
        : "memory"
    );
    return ((uint64_t)hi << 32) | lo;
}

// ============================================================
// HIGH-SPEED HARDWARE OVERFLOW HANDLER
// ============================================================

static bool IRAM_ATTR on_hf_overflow(pcnt_unit_handle_t unit, const pcnt_watch_event_data_t *edata, void *user_ctx) {
    if (edata->watch_point_value == ACCUMULATE_LIMIT) {
        hf_overflow_count += ACCUMULATE_LIMIT;
    }
    return false;
}

// ============================================================
// 1PPS INTERRUPT - 64-bit CPU CYCLE COUNTER (2.78ns resolution)
// ============================================================

void IRAM_ATTR on1PPS_Interrupt() {
    uint64_t current_cycles = read_mcycle_64();  // 2.78ns resolution!
    int current_hf_raw = 0;
    
    pcnt_unit_get_count(hf_signal_unit, &current_hf_raw);
    uint64_t current_hf_total = (uint64_t)(hf_overflow_count + current_hf_raw);
    
    delta_hf_pulses = current_hf_total - last_hf_total_snapshot;
    delta_reference_cycles = current_cycles - last_gate_cycle;
    
    last_hf_total_snapshot = current_hf_total;
    last_gate_cycle = current_cycles;
    
    loop_measurement_ready = true;
}

// ============================================================
// BUTTON INTERRUPTS
// ============================================================

void IRAM_ATTR isr_mode_button() {
    static unsigned long last_interrupt_time = 0;
    unsigned long interrupt_time = millis();
    if (interrupt_time - last_interrupt_time > 250) { 
        flag_mode_pressed = true;
    }
    last_interrupt_time = interrupt_time;
}

void IRAM_ATTR isr_up_button() {
    static unsigned long last_interrupt_time = 0;
    unsigned long interrupt_time = millis();
    if (interrupt_time - last_interrupt_time > 100) {
        flag_up_pressed = true;
    }
    last_interrupt_time = interrupt_time;
}

void IRAM_ATTR isr_down_button() {
    static unsigned long last_interrupt_time = 0;
    unsigned long interrupt_time = millis();
    if (interrupt_time - last_interrupt_time > 100) {
        flag_down_pressed = true;
    }
    last_interrupt_time = interrupt_time;
}



// ============================================================
// SETUP
// ============================================================

void setup() {
    setCpuFrequencyMhz(360);
    
    pinMode(PIN_BTN_UP, INPUT_PULLUP);
    pinMode(PIN_BTN_DOWN, INPUT_PULLUP);
    pinMode(PIN_BTN_MODE, INPUT_PULLUP); 
    pinMode(PIN_1PPS_GATE, INPUT_PULLUP);
    
    SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
    tft.init(TFT_WIDTH, TFT_HEIGHT);
    tft.setRotation(3);
    tft.fillScreen(ST77XX_BLACK);

    tft.setCursor(10, 50);
    tft.setTextColor(ST77XX_CYAN, ST77XX_BLACK);
    tft.setTextSize(2);
    tft.print("GPS DISCIPLINED COUNTER");
    tft.setCursor(10, 90);
    tft.setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
    tft.print("Waiting for GPS...");
    tft.setCursor(10, 120);
    tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
    tft.print("Resolution: 2.78 ns");

    // ============================================================
    // PCNT SETUP
    // ============================================================
    
    pcnt_unit_config_t hf_unit_config = {
        .low_limit = -ACCUMULATE_LIMIT,
        .high_limit = ACCUMULATE_LIMIT,
    };
    ESP_ERROR_CHECK(pcnt_new_unit(&hf_unit_config, &hf_signal_unit));

    pcnt_chan_config_t hf_chan_config = {
        .edge_gpio_num = PIN_HF_INPUT, 
        .level_gpio_num = -1,
    };
    ESP_ERROR_CHECK(pcnt_new_channel(hf_signal_unit, &hf_chan_config, &hf_signal_chan));
    
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(hf_signal_chan, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_HOLD));
    ESP_ERROR_CHECK(pcnt_unit_add_watch_point(hf_signal_unit, ACCUMULATE_LIMIT));

    pcnt_event_callbacks_t hf_cbs = { .on_reach = on_hf_overflow };
    ESP_ERROR_CHECK(pcnt_unit_register_event_callbacks(hf_signal_unit, &hf_cbs, NULL));
    ESP_ERROR_CHECK(pcnt_unit_enable(hf_signal_unit));

    int initial_hf_raw = 0;
    pcnt_unit_get_count(hf_signal_unit, &initial_hf_raw);
    last_hf_total_snapshot = initial_hf_raw;
    
    // Initialize 64-bit cycle counter
    last_gate_cycle = read_mcycle_64();
    
    // Attach interrupts
    attachInterrupt(digitalPinToInterrupt(PIN_BTN_MODE), isr_mode_button, FALLING);
    attachInterrupt(digitalPinToInterrupt(PIN_BTN_UP), isr_up_button, FALLING);
    attachInterrupt(digitalPinToInterrupt(PIN_BTN_DOWN), isr_down_button, FALLING);
    attachInterrupt(digitalPinToInterrupt(PIN_1PPS_GATE), on1PPS_Interrupt, RISING);
    
    pcnt_unit_start(hf_signal_unit);
    
    // Wait for first 1PPS
    unsigned long timeout = millis() + 10000;
    while (last_gate_cycle == 0 && millis() < timeout) {
        delay(10);
    }
    delay(2000);  //show start msg if no gps wait

    tft.fillScreen(ST77XX_BLACK);
}

// ============================================================
// DISPLAY FUNCTIONS
// ============================================================

double aggregated_avg = 0.0;
double old_display_hz = -1.0;
double old_display_avg = -1.0;
uint32_t old_display_samples = 0;
double old_display_target = -1.0;
bool old_display_mode_state = true;

void renderLineBG(int x, int y, const char* prefix, double val, int decimals, uint16_t txt_color, uint8_t txt_size, const char* suffix) {
    tft.setCursor(x, y);
    tft.setTextSize(txt_size);
    tft.setTextColor(txt_color, ST77XX_BLACK); 
    tft.print(prefix);
    tft.print(val, decimals);
    tft.print(suffix);
    tft.print("                 "); 
}

void renderStringBG(int x, int y, const char* prefix, const char* payload, uint16_t txt_color, uint8_t txt_size) {
    tft.setCursor(x, y);
    tft.setTextSize(txt_size);
    tft.setTextColor(txt_color, ST77XX_BLACK); 
    tft.print(prefix);
    tft.print(payload);
    tft.print("                 "); 
}

void displayModePropertiesGuide(bool mode_target_state) {
    tft.fillScreen(ST77XX_BLACK);
    tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
    tft.drawFastHLine(10, 38, TFT_HEIGHT - 20, ST77XX_WHITE);
    tft.setTextSize(2);
    tft.setCursor(10, 15);

    if (!mode_target_state) {
        tft.print("MODE: COUNTER\n\n");
        tft.print("*Measures input signal\n\n");
        tft.print("    frequencies over 1 sec\n\n");
        tft.print("*Real-time instant update\n");
    } else {
        tft.print("MODE: CRYSTAL TUNER\n\n");
        tft.print("*Locked base target, so\n\n");
        tft.print("   no edge jitter errors\n\n");
        tft.print("*UP/DN key tuning control\n");
    }
    
    for (byte i=9; i>0; i--){
        tft.setCursor(10, TFT_WIDTH - 26);
        tft.print("Resuming in "); tft.print(i); 
        delay(1000); 
    }
    old_display_hz = -1.0;
    old_display_avg = -1.0;
    old_display_samples = 0;
    old_display_target = -1.0;
    force_complete_redraw_after_menu = true;
}

// ============================================================
// PRECISION RECIPROCAL CALCULATION MODEL
// ============================================================
// ============================================================
// UPDATED DISPLAY FUNCTIONS - WITH PPM IN TUNER MODE
// ============================================================

// ============================================================
// GLOBAL VARIABLES FOR PPM
// ============================================================

double current_ppm = 0.0;
double old_display_ppm = 9999.0;

// ============================================================
// UPDATED run_comparison_math()
// ============================================================

void run_comparison_math() {

/* --- TUNER MODE: Check for >1% frequency change ---
if (tuning_mode_active && delta_hf_pulses > 0) {
    // Calculate current measured frequency (same as counter mode)
    double elapsed_seconds = (double)delta_reference_cycles / (double)CPU_FREQ;
    double measured_freq = (double)delta_hf_pulses / elapsed_seconds;
    
    // Check if deviation from target is > 1% (10000 PPM)
    double deviation_percent = fabs(measured_freq - user_tuned_target_baseline) / user_tuned_target_baseline * 100.0;
    
    if (deviation_percent > 1.0) {
        // Fall back to Counter mode
        tuning_mode_active = false;
        sample_index = 0;
        aggregated_avg = 0.0;
        old_display_hz = -1.0;
        old_display_avg = -1.0;
        old_display_samples = 0;
        old_display_target = -1.0;
        old_display_ppm = 9999.0;
        force_complete_redraw_after_menu = true;
        
        // Update target to new frequency
        user_tuned_target_baseline = round(measured_freq);
        
        // Briefly show the mode change on display
        tft.fillScreen(ST77XX_BLACK);
        tft.setCursor(10, 50);
        tft.setTextColor(ST77XX_ORANGE, ST77XX_BLACK);
        tft.setTextSize(2);
        tft.print("AUTO SWITCH");
        tft.setCursor(10, 90);
        tft.setTextSize(1);
        tft.print(">1% change detected");
        tft.setCursor(10, 110);
        tft.print("Switched to Counter Mode");
        delay(1000);
        force_complete_redraw_after_menu = true;
        
        // Recalculate frequency in counter mode (skip rest of function)
        // The next PPS will handle the measurement
        return;
    }*/

    if (delta_reference_cycles == 0) return;

    double exact_calculated_frequency = 0.0;
    
    // Convert CPU cycles to seconds
    double elapsed_seconds = (double)delta_reference_cycles / (double)CPU_FREQ;

    if (!tuning_mode_active) {
        // --- MODE A: STANDARD RECIPROCAL SEARCH ---
        exact_calculated_frequency = (double)delta_hf_pulses / elapsed_seconds;
        
        if (exact_calculated_frequency > 0.0 && delta_hf_pulses > 0) {
            user_tuned_target_baseline = round(exact_calculated_frequency);
        }
        current_ppm = 0.0;  // No PPM in counter mode
    } else {
        // --- MODE B: CRYSTAL TUNER MODE ---
        double target_pps_duration = 1.0;
        double actual_gate_duration = elapsed_seconds;
        exact_calculated_frequency = user_tuned_target_baseline * (target_pps_duration / actual_gate_duration);
        
        // Calculate PPM deviation from target
        current_ppm = ((exact_calculated_frequency - user_tuned_target_baseline) / user_tuned_target_baseline) * 1000000.0;
    }

    if (delta_hf_pulses == 0) {
        exact_calculated_frequency = 0.0;
        current_ppm = 0.0;
    }

    sample_index++;
    if (sample_index == 1) {
        aggregated_avg = exact_calculated_frequency;
    } else {
        double variance_check = fabs(exact_calculated_frequency - aggregated_avg) / aggregated_avg;
        if (variance_check > 0.01) { 
            aggregated_avg = exact_calculated_frequency;
            sample_index = 1;
        } else {
            aggregated_avg = aggregated_avg + ((exact_calculated_frequency - aggregated_avg) / (double)sample_index);
        }
    }

    int line_height = 28;
    int margin_y = 15;
    byte dp = 6;

    if (force_complete_redraw_after_menu) {
        force_complete_redraw_after_menu = false;
        tft.fillScreen(ST77XX_BLACK);
        tft.drawRect(0,5,319,164,ST77XX_BLUE);
    }
    
    if (exact_calculated_frequency > 10000) dp = 4;
    if (exact_calculated_frequency > 1000000) dp = 2;

    // --- LINE 1: Freq output ---
    if (exact_calculated_frequency != old_display_hz) {
        old_display_hz = exact_calculated_frequency;
        renderLineBG(10, margin_y, "Freq: ", exact_calculated_frequency, dp, ST77XX_GREEN, 2, " Hz");
    }

    // --- LINE 2: Base Target ---
   /* if (user_tuned_target_baseline != old_display_target || delta_hf_pulses == 0) {
        old_display_target = user_tuned_target_baseline;
        if (delta_hf_pulses > 0) {
            renderLineBG(10, margin_y + line_height * 1.2, "Target: ", user_tuned_target_baseline, 0, ST77XX_YELLOW, 2, " Hz");
        } else {
            renderStringBG(10, margin_y + line_height * 1.2, "Target: ", "N/A", ST77XX_YELLOW, 2);
        }
        if (tuning_mode_active) { 
            tft.drawFastHLine(240, 58, 55, ST77XX_YELLOW);
            tft.fillTriangle(280,46,290,46,285,36,ST77XX_YELLOW);
            tft.fillTriangle(280,70,290,70,285,80,ST77XX_YELLOW);
    
        }
    }*/// --- LINE 2: Base Target ---
if (user_tuned_target_baseline != old_display_target || delta_hf_pulses == 0) {
    old_display_target = user_tuned_target_baseline;
    
    // Clear the area
    tft.fillRect(100, margin_y + line_height * 1.2, 80, 24, ST77XX_BLACK);
    tft.setCursor(10, margin_y + line_height * 1.2);
    tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
    tft.setTextSize(2);
    tft.print("Target: ");
    
    // Always show target in Tuner mode (even with no pulses)
    // In Counter mode, show target with parentheses in red if no pulses
    if (tuning_mode_active) {
        // Tuner mode: always show target in yellow
        tft.setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
        tft.print(user_tuned_target_baseline, 0);
        tft.print(" Hz   ");
    } else if (delta_hf_pulses > 0) {
        // Counter mode with pulses: show target in yellow
        tft.setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
        tft.print(user_tuned_target_baseline, 0);
        tft.print(" Hz   ");
    } else {
        // Counter mode with no pulses: show target in red with parentheses
        tft.setTextColor(ST77XX_RED, ST77XX_BLACK);
        tft.print("(");
        tft.print(user_tuned_target_baseline, 0);
        tft.print(" Hz)   ");
    }
    
    if (tuning_mode_active) { 
        tft.drawFastHLine(240, 58, 55, ST77XX_YELLOW);
        tft.fillTriangle(280,46,290,46,285,36,ST77XX_YELLOW);
        tft.fillTriangle(280,70,290,70,285,80,ST77XX_YELLOW);
    }
}
    // --- LINE 3: Average ---
    if (aggregated_avg != old_display_avg) {
        old_display_avg = aggregated_avg;
        renderLineBG(10, margin_y + line_height * 2.4, "Avg:   ", aggregated_avg, dp, ST77XX_CYAN, 2, " Hz  ");
       
    }// --- LINE 4: Samples---
       if (sample_index != old_display_samples) {
            old_display_samples = sample_index;
            // Clear the line (from x=120 to end)
          //  tft.fillRect(120, margin_y + line_height * 3.6, 70, 24, ST77XX_BLACK);
            tft.setCursor(10, margin_y + line_height * 3.6);
            tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
            tft.setTextSize(2);
            tft.print("Samples: ");
            tft.print(sample_index);
    // --- LINE 5: Samples & PPM (Tuner Mode) ---
    if (tuning_mode_active) {
       // --- Check for >1% frequency change (WARNING ONLY) ---
    if (delta_hf_pulses > 0) {
    // Calculate current measured frequency (same as counter mode)
    double elapsed_seconds = (double)delta_reference_cycles / (double)CPU_FREQ;
    double measured_freq = (double)delta_hf_pulses / elapsed_seconds;
    
    // Check if deviation from target is > 1%
    double deviation_percent = fabs(measured_freq - user_tuned_target_baseline) / user_tuned_target_baseline * 100.0;
    
    // Only flag if deviation > 1% AND target is above 10 Hz (avoid low-frequency noise)
    bool target_lost = (deviation_percent > 1.0 /*&& user_tuned_target_baseline > 10.0*/);
    
    // Store the state for display
    static bool target_lost_state = false;
    if (target_lost != target_lost_state) {
        target_lost_state = target_lost;
        // Force display update on next cycle
        old_display_target = -1.0;  // This will trigger a redraw of the Target line
    }
    }
        // Format PPM: -999.9 to +999.9, or "OVF" if outside range
        char ppm_buffer[12];
        
        if (fabs(current_ppm) > 999.9) {
            strcpy(ppm_buffer, "OVF");
        } else {
            // Show PPM with 1 decimal place
            dtostrf(current_ppm, 7, 1, ppm_buffer);
        }
        
        // Only update if changed
        if (current_ppm != old_display_ppm) {
            old_display_ppm = current_ppm;
            // Clear the line (from x=120 to end)
            //tft.fillRect(120, margin_y + line_height * 3.6, 60, 24, ST77XX_BLACK);
            tft.setCursor(10, margin_y + line_height * 4.8);
            tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
            tft.setTextSize(2);
            tft.print("PPM: ");
            
            // Color code the PPM value
            float ppm_abs = fabs(current_ppm);
            if (ppm_abs < 0.5) {
                tft.setTextColor(ST77XX_GREEN, ST77XX_BLACK);
            } else if (ppm_abs < 5.0) {
                tft.setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
            } else if (ppm_abs < 50.0) {
                tft.setTextColor(ST77XX_ORANGE, ST77XX_BLACK);
            } else {
                tft.setTextColor(ST77XX_RED, ST77XX_BLACK);
            }
            
            if (strcmp(ppm_buffer, "OVF") == 0) {
                tft.print(" OVF");
            } else {
                tft.print(ppm_buffer);
                tft.print(" PPM");
            }
        }
    } else {
        // --- COUNTER MODE: Show Samples ---
       
            tft.setCursor(10, margin_y + line_height * 4.8);
            tft.print("MODE: FREQUENCY COUNTER");
        }
        // Reset old_display_ppm so it updates when switching to tuner mode
        old_display_ppm = 9999.0;
    }
}

void run_comparison_math2() {
    if (delta_reference_cycles == 0) return;

    double exact_calculated_frequency = 0.0;
    
    // Convert CPU cycles to seconds
    double elapsed_seconds = (double)delta_reference_cycles / (double)CPU_FREQ;

    if (!tuning_mode_active) {
        // --- MODE A: STANDARD RECIPROCAL SEARCH ---
        exact_calculated_frequency = (double)delta_hf_pulses / elapsed_seconds;
        
        if (exact_calculated_frequency > 0.0 && delta_hf_pulses > 0) {
            user_tuned_target_baseline = round(exact_calculated_frequency);
        }
    } else {
        // --- MODE B: SUB-PPM FIXED BASE TUNING RATIO ---
        double target_pps_duration = 1.0;
        double actual_gate_duration = elapsed_seconds;
        exact_calculated_frequency = user_tuned_target_baseline * (target_pps_duration / actual_gate_duration);
    }

    if (delta_hf_pulses == 0) exact_calculated_frequency = 0.0;

    sample_index++;
    if (sample_index == 1) {
        aggregated_avg = exact_calculated_frequency;
    } else {
        double variance_check = fabs(exact_calculated_frequency - aggregated_avg) / aggregated_avg;
        if (variance_check > 0.01) { 
            aggregated_avg = exact_calculated_frequency;
            sample_index = 1;
        } else {
            aggregated_avg = aggregated_avg + ((exact_calculated_frequency - aggregated_avg) / (double)sample_index);
        }
    }

    int line_height = 28;
    int margin_y = 15;
    byte dp = 6;

    if (force_complete_redraw_after_menu) {
        force_complete_redraw_after_menu = false;
        tft.fillScreen(ST77XX_BLACK);
    }
    
    if (exact_calculated_frequency > 10000) dp = 4;
    if (exact_calculated_frequency > 1000000) dp = 2;

    // Line 1: Freq output
    if (exact_calculated_frequency != old_display_hz) {
        old_display_hz = exact_calculated_frequency;
        renderLineBG(10, margin_y, "Freq: ", exact_calculated_frequency, dp, ST77XX_GREEN, 2, " Hz");
    }

    // Line 2: Base Target// --- LINE 2: Base Target ---
if (user_tuned_target_baseline != old_display_target || delta_hf_pulses == 0) {
    old_display_target = user_tuned_target_baseline;
    
    // Only show "N/A" in COUNTER MODE when delta_hf_pulses == 0
    // In TUNER MODE, always show the target value
    if (delta_hf_pulses > 0) {
        renderLineBG(10, margin_y + line_height * 1.2, "Target: ", user_tuned_target_baseline, 0, ST77XX_YELLOW, 2, " Hz");
    } else {
        // Only show "N/A" in COUNTER mode, show target in TUNER mode
        if (tuning_mode_active) {
            // In tuner mode, show the target even if no pulses
            renderLineBG(10, margin_y + line_height * 1.2, "Target: ", user_tuned_target_baseline, 0, ST77XX_YELLOW, 2, " Hz");
        } else {
            renderStringBG(10, margin_y + line_height * 1.2, "Target: ", "N/A", ST77XX_YELLOW, 2);
        }
    }
    
    if (tuning_mode_active) { 
        tft.drawFastHLine(240, 58, 55, ST77XX_YELLOW);
        tft.fillTriangle(280,46,290,46,285,36,ST77XX_YELLOW);
        tft.fillTriangle(280,70,290,70,285,80,ST77XX_YELLOW);
    }
}
    if (user_tuned_target_baseline != old_display_target || delta_hf_pulses == 0) {
        old_display_target = user_tuned_target_baseline;
        if (delta_hf_pulses > 0) {
            renderLineBG(10, margin_y + line_height * 1.2, "Target: ", user_tuned_target_baseline, 0, ST77XX_YELLOW, 2, " Hz");
        } else {
            renderStringBG(10, margin_y + line_height * 1.2, "Target: ", "N/A", ST77XX_YELLOW, 2);
        }
    }

    // Line 3: Average
    if (aggregated_avg != old_display_avg) {
        old_display_avg = aggregated_avg;
        renderLineBG(10, margin_y + line_height * 2.4, "Avg:   ", aggregated_avg, dp, ST77XX_CYAN, 2, " Hz");
    }

    // Line 4: Samples
    if (sample_index != old_display_samples) {
        old_display_samples = sample_index;
        renderLineBG(10, margin_y + line_height * 3.6, "Samples: ", (double)sample_index, 0, ST77XX_WHITE, 2, "");
    }
 if (tuning_mode_active) {
        tft.fillTriangle(280,46,290,46,285,36,ST77XX_YELLOW);tft.fillTriangle(280,70,290,70,285,80,ST77XX_YELLOW);
        //tft.fillRect(280,36,10,10,ST77XX_YELLOW);tft.fillRect(280,70,10,10,ST77XX_YELLOW);
    }
    // Line 5: Mode status
    if (tuning_mode_active != old_display_mode_state) {
        old_display_mode_state = tuning_mode_active;
        tft.setCursor(10, margin_y + line_height * 4.8);
        tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
        if (tuning_mode_active) {
            tft.print("MANUAL CRYSTAL TUNER   ");
        } else {
            tft.print("MODE: FREQUENCY COUNTER");
        }
    }
}

// ============================================================
// BUTTON HANDLER
// ============================================================
void handleAsyncInterruptEvents() {
    if (flag_mode_pressed) {
        flag_mode_pressed = false;
        tuning_mode_active = !tuning_mode_active;
        sample_index = 0;
        displayModePropertiesGuide(tuning_mode_active);
        loop_measurement_ready = false;
    }

    if (tuning_mode_active) {
        // --- Check if buttons are still held ---
        // This re-triggers the press if the button is still down
        if (digitalRead(PIN_BTN_UP) == LOW) {
            // Button is still held - set flag again (debounced)
            static unsigned long last_up_check = 0;
            unsigned long current_time = millis();
            
            // Calculate delay based on repeat count (acceleration)
            unsigned long delay_ms = 500;
            if (up_repeat_count > 50) delay_ms = 50;
            else if (up_repeat_count > 30) delay_ms = 100;
            else if (up_repeat_count > 15) delay_ms = 150;
            else if (up_repeat_count > 5) delay_ms = 250;
            
            if (current_time - last_up_check > delay_ms) {
                last_up_check = current_time;
                flag_up_pressed = true;  // Re-trigger
            }
        } else {
            up_repeat_count = 0;
        }
        
        if (digitalRead(PIN_BTN_DOWN) == LOW) {
            static unsigned long last_down_check = 0;
            unsigned long current_time = millis();
            
            unsigned long delay_ms = 500;
            if (down_repeat_count > 50) delay_ms = 50;
            else if (down_repeat_count > 30) delay_ms = 100;
            else if (down_repeat_count > 15) delay_ms = 150;
            else if (down_repeat_count > 5) delay_ms = 250;
            
            if (current_time - last_down_check > delay_ms) {
                last_down_check = current_time;
                flag_down_pressed = true;
            }
        } else {
            down_repeat_count = 0;
        }
        
        // --- Process UP button ---
        if (flag_up_pressed) {
            flag_up_pressed = false;
            unsigned long current_time = millis();
            
            uint32_t step = 1;
            if (up_repeat_count > 50) step = 100;
            else if (up_repeat_count > 30) step = 50;
            else if (up_repeat_count > 15) step = 10;
            else if (up_repeat_count > 5) step = 5;
            
            user_tuned_target_baseline += step;
            sample_index = 0;
            up_repeat_count++;
            last_up_press_time = current_time;
        }
        
        // --- Process DOWN button ---
        if (flag_down_pressed) {
            flag_down_pressed = false;
            unsigned long current_time = millis();
            
            uint32_t step = 1;
            if (down_repeat_count > 50) step = 100;
            else if (down_repeat_count > 30) step = 50;
            else if (down_repeat_count > 15) step = 10;
            else if (down_repeat_count > 5) step = 5;
            
            if (user_tuned_target_baseline > step) {
                user_tuned_target_baseline -= step;
            } else {
                user_tuned_target_baseline = 1.0;
            }
            sample_index = 0;
            down_repeat_count++;
            last_down_press_time = current_time;
        }
    } else {
        flag_up_pressed = false;
        flag_down_pressed = false;
    }
}
void handleAsyncInterruptEvents2() {
  if (flag_mode_pressed) {
        flag_mode_pressed = false;
        tuning_mode_active = !tuning_mode_active;
        sample_index = 0;
        displayModePropertiesGuide(tuning_mode_active);
        loop_measurement_ready = false;
    }
  /*
    if (tuning_mode_active) {
        if (flag_up_pressed) {
            flag_up_pressed = false;
            user_tuned_target_baseline += 1.0;
            sample_index = 0;
        }
        if (flag_down_pressed) {
            flag_down_pressed = false;
            if (user_tuned_target_baseline > 1.0) user_tuned_target_baseline -= 1.0;
            sample_index = 0;
        }
    } else {
        flag_up_pressed = false;
        flag_down_pressed = false;
    }*/
    if (tuning_mode_active) {
    // --- UP BUTTON with repeat and acceleration ---
    if (flag_up_pressed) {
        flag_up_pressed = false;
        unsigned long current_time = millis();
        
        // Calculate step size based on repeat count (accelerating)
        uint32_t step = 1;
        if (up_repeat_count > 50) {
            step = 100;   // Max speed: 100 Hz per step
        } else if (up_repeat_count > 30) {
            step = 50;
        } else if (up_repeat_count > 15) {
            step = 10;
        } else if (up_repeat_count > 5) {
            step = 5;
        } else {
            step = 1;
        }
        
        user_tuned_target_baseline += step;
        sample_index = 0;
        up_repeat_count++;
        last_up_press_time = current_time;
    }
    
    // --- DOWN BUTTON with repeat and acceleration ---
    if (flag_down_pressed) {
        flag_down_pressed = false;
        unsigned long current_time = millis();
        
        // Calculate step size based on repeat count (accelerating)
        uint32_t step = 1;
        if (down_repeat_count > 50) {
            step = 100;   // Max speed: 100 Hz per step
        } else if (down_repeat_count > 30) {
            step = 50;
        } else if (down_repeat_count > 15) {
            step = 10;
        } else if (down_repeat_count > 5) {
            step = 5;
        } else {
            step = 1;
        }
        
        if (user_tuned_target_baseline > step) {
            user_tuned_target_baseline -= step;
        } else {
            user_tuned_target_baseline = 1.0;
        }
        sample_index = 0;
        down_repeat_count++;
        last_down_press_time = current_time;
    }
} else {
    flag_up_pressed = false;
    flag_down_pressed = false;
}

// --- Reset repeat counters if buttons are released (no press for 500ms) ---
if (tuning_mode_active) {
    unsigned long current_time = millis();
    if (current_time - last_up_press_time > 500) {
        up_repeat_count = 0;
    }
    if (current_time - last_down_press_time > 500) {
        down_repeat_count = 0;
    }
}
}

// ============================================================
// MAIN LOOP
// ============================================================

void loop() {
    handleAsyncInterruptEvents();

    if (loop_measurement_ready) {
        loop_measurement_ready = false;
        run_comparison_math();
    }
    
    delay(5);
}