// -----------------------------------------------------------------------------------
// ******* Arduino compatible analogWrite() and tone() functions for the ESP32 *******
// Takes over 8 of the 16 PWM channels (default, uses channel# 8 to 15) so only channel#'s < 8 can be used elsewhere.
// The 8 PWM channels are shared between the non-blocking tone() and analogWrite() functions and are automatically allocated as required.

portMUX_TYPE __analogOutMux = portMUX_INITIALIZER_UNLOCKED;

#define __ANALOG_PWM_CHANNEL_START 8  // set aside 8 channels for use elsewhere, use 0 to dedicate all 16 channels here

volatile uint8_t __tone_p = 0;
volatile unsigned int __tone_f = 0;
volatile unsigned long __tone_d = 0;
volatile int __channelMap[16] = {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1};
TaskHandle_t __toneTask;

// find and disable any PWM already on this pin
void __pwmDeallocateChannel(int pin) {
  for (int channel=__ANALOG_PWM_CHANNEL_START; channel<15; channel++) {
    if (__channelMap[channel]==pin) { ledcDetachPin(pin); ledcWrite(channel,0); __channelMap[channel]=-1; break; }
  }
}

// find a PWM channel for this pin, returns -1 if no channel is available
int __pwmAllocateChannel(int pin, boolean clearExisting=false) {
  if (clearExisting) __pwmDeallocateChannel(pin);
  for (int channel=__ANALOG_PWM_CHANNEL_START; channel<15; channel++) {
    if (__channelMap[channel]==-1) { __channelMap[channel]=pin; return channel; }
  }
  log_e("PWM, no channels available.");
  return -1;
}

int __pwmGetChannel(int pin) {
  for (int channel=__ANALOG_PWM_CHANNEL_START; channel<15; channel++) { if (__channelMap[channel]==pin) return channel; }
  return -1;
}

// tone()/noTone() rewritten to use the ESP32 LEDC driver directly  2026,10/21  NS3000
// History: with ESP32 core 2.0.17 the original code (ledcAttachPin inside a critical section) aborted right after boot.
//       Moving it out of the critical section and changing the order stopped the abort, but no sound came out (boot beep, GOTO beep, :=SD#).
// New approach: set up a dedicated buzzer timer and channel, sound it by setting the duty to 50%,
//       and set the duty back to 0 with esp_timer when the time is up. No channel allocation, pin attach/detach or task creation per call.
//       The channel is low-speed group channel 7 (Arduino number 15). It does not collide with 8..14 used by analogWrite() below.
#include "driver/ledc.h"
#include "esp_timer.h"
#define __TONE_MODE   LEDC_LOW_SPEED_MODE
#define __TONE_TIMER  LEDC_TIMER_3
#define __TONE_CH     LEDC_CHANNEL_7
static int __tonePin=-1;
static esp_timer_handle_t __toneTimer=NULL;

static void __toneOff(void *arg) {
  ledc_set_duty(__TONE_MODE,__TONE_CH,0);
  ledc_update_duty(__TONE_MODE,__TONE_CH);
}

// No default value "= 0" here (the declaration in Arduino.h of ESP32 core 2.0.17 already has one)
__attribute__ ((weak)) void tone(uint8_t pin, unsigned int frequency, unsigned long duration)
{
  if (frequency == 0) { __toneOff(NULL); return; }
  if (__toneTimer != NULL) esp_timer_stop(__toneTimer);
  {  // set the timer and channel again on every call (using only ledc_set_freq from the 2nd call on gave no sound)
    ledc_timer_config_t t = {};
    t.speed_mode=__TONE_MODE; t.duty_resolution=LEDC_TIMER_10_BIT; t.timer_num=__TONE_TIMER; t.freq_hz=frequency; t.clk_cfg=LEDC_AUTO_CLK;
    if (ledc_timer_config(&t) != ESP_OK) return;
    ledc_channel_config_t c = {};
    c.gpio_num=pin; c.speed_mode=__TONE_MODE; c.channel=__TONE_CH; c.intr_type=LEDC_INTR_DISABLE; c.timer_sel=__TONE_TIMER; c.duty=0; c.hpoint=0;
    if (ledc_channel_config(&c) != ESP_OK) return;
    if (__toneTimer == NULL) {
      esp_timer_create_args_t a = {};
      a.callback=__toneOff; a.name="tone";
      if (esp_timer_create(&a,&__toneTimer) != ESP_OK) __toneTimer=NULL;
    }
    __tonePin=pin;
  }
  ledc_set_duty(__TONE_MODE,__TONE_CH,512);  // 50% (10 bit)
  ledc_update_duty(__TONE_MODE,__TONE_CH);
  if (duration && __toneTimer != NULL) esp_timer_start_once(__toneTimer,(uint64_t)duration*1000ULL);
}

__attribute__ ((weak)) void noTone(uint8_t pin)
{
  if (__toneTimer != NULL) esp_timer_stop(__toneTimer);
  if (__tonePin == pin) __toneOff(NULL);
}

__attribute__ ((weak)) void analogWrite(uint8_t pin, int value) {
  portENTER_CRITICAL(&__analogOutMux);
  int channel=__pwmAllocateChannel(pin,true);  // using option to clear/detach any existing channel for this pin
  if (channel>=0) {
    ledcSetup(channel,1000,8); // 1000Hz, 8 bit
    ledcAttachPin(pin,channel);
    ledcWrite(channel,value);
    // just disconnect and go digital if value is 0 or 255
    if (value==0) { noTone(pin); digitalWrite(pin,LOW); }
    if (value==255) { noTone(pin); digitalWrite(pin,HIGH); }
  } else log_e("analogWrite, PWM channel in use or unavailable.");
  portEXIT_CRITICAL(&__analogOutMux);
}
