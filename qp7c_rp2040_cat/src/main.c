/*
 * Copyright (C) 2023- Hitoshi Kawaji <je1rav@gmail.com>
 * 
 * The digital mode tranceiver QP-7C RP2040 with CAT control (Simulating
 * TS2000)
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include "bsp/board.h"
#include "tusb.h"
#include "usb_descriptors.h"
#include "pico/stdlib.h"
#include "pico/binary_info.h"
#include "hardware/gpio.h"
#include "hardware/adc.h"
#include "hardware/i2c.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"

#include <hardware/watchdog.h>    //watchdog

#include "usb_audio.h"

// ====superheterodyne==============================
//#define Superheterodyne
// =================================================
// if #define, it will be a superheterodyne receiver.  
// if not #define, it will be a direct conversion receiver. 

#define SI5351_CAL 0         // Calibrate for your Si5351 module.
#define FREQ_BFO 446400      // in Hz: Calibrate for your Filter. (if not #define Superheterodyne, no meaning.)
#define AUDIOSAMPLING 48000  // USB Audio sampling frequency (fixed)

#define N_FREQ 2 // number of using RF frequencies with push switch　(<= 7)
#define FREQ_0 7041000 // RF frequency in Hz
#define FREQ_1 7074000 // in Hz
//#define FREQ_2 7074000 // in Hz
//#define FREQ_3 7074000 // in Hz
//#define FREQ_4 7074000 // in Hz
//#define FREQ_5 7074000 // in Hz
//#define FREQ_6 7074000 // in Hz
uint64_t Freq_table[N_FREQ]={FREQ_0,FREQ_1}; // Freq_table[N_FREQ]={FREQ_0,FREQ_1, ...}

//pins
#define pin_A0 26U //pin for ADC (A0)
#define pin_RX 27U //pin for RX switch (D1,output)
#define pin_TX 28U //pin for TX switch (D2,output)
#define pin_SW 3U //pin for freq change switch (D10,input)
#define pin_RED 17U //pin for Red LED (output)
#define pin_GREEN 16U //pin for GREEN LED (output)
//#define pin_BLUE 25U //pin for BLUE LED (output) 
#define pin_LED_POWER 11U //pin for NEOPIXEL LED power (output)
#define WS2812_PIN 12U //pin for NEOPIXEL LED (output)
#define I2C_SDA_PIN 6U  //pin for I2C SDA
#define I2C_SCL_PIN 7U  //pin for I2C SCL

#define ONBOARD_LED_ON  0  //on board LED. In the case of Xiao RP2040, O:on, 1:off
#define ONBOARD_LED_OFF 1  //on board LED. In the case of Xiao RP2040, O:on, 1:off

#define I2C_PORT      i2c1  //i2c port (i2c0 or i2c1) depending of using I2C pins 

#define BRIGHTNESS 5   //brightness of NEOPIXEL LED (max 255)
#define IS_RGBW true

//--------------------------------------------------------------------+
// Si5351: NT7S "si5351" Library ”https://github.com/NT7S/Si5351" is used 
// with some modifications.
#include "si5351.h"  

//--------------------------------------------------------------------+
//Neo Pixel from "Pico-examples/pio/ws2812"
#include "ws2812.pio.h" 
static inline void put_pixel(uint32_t pixel_grb) {
    pio_sm_put_blocking(pio0, 0, pixel_grb << 8u);
}
static inline uint32_t urgb_u32(uint8_t r, uint8_t g, uint8_t b) {
    return
            ((uint32_t) (r) << 8) |
            ((uint32_t) (g) << 16) |
            (uint32_t) (b);
}

//NeoPixel 7 color patterns + off (indication of CAT control)
typedef struct color_record {
         uint8_t id;
         uint8_t data[3];
} RECORD;
RECORD pixel_color[8] = {{0, {BRIGHTNESS, 0,0}},  
                         {1, {0,BRIGHTNESS, 0}},
                         {2, {0, 0, BRIGHTNESS}},
                         {3, {BRIGHTNESS, BRIGHTNESS, 0}},
                         {4, {BRIGHTNESS, 0, BRIGHTNESS}},
                         {5, {0, BRIGHTNESS, BRIGHTNESS}},
                         {6, {BRIGHTNESS, BRIGHTNESS, BRIGHTNESS}},
                         {7, {0, 0, 0}}                  
                         };

//for ADC offset at trecieving
int32_t adc_offset = 0;   

//for tranceiver
uint64_t RF_freq;   // RF frequency (Hz)
#ifdef Superheterodyne
int64_t BFO_freq = FREQ_BFO;   // BFO frequency (Hz)
#else
int64_t BFO_freq = 0;
#endif
int C_freq = 0;    // FREQ_x: Index of RF frequency. In this case, FREQ_0 is selected as the initial frequency.
int Tx_Status = 0; // 0=RX, 1=TX
int Tx_Start = 0;  // 0=RX, 1=TX
int not_TX_first = 0;
uint32_t Tx_last_mod_time;
uint32_t Tx_last_time;
uint32_t push_last_time;  // to detect the long puch for frequency change by push switch

//for determination of Audio signal frequency 
int16_t mono_prev=0;  
int16_t mono_preprev=0;  
float delta_prev=0;
int16_t sampling=0;
int16_t cycle=0;
uint32_t cycle_frequency[136];

//for USB Audio
int16_t monodata[CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ / 4];
int16_t adc_data[CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ / 2];
int16_t pcCounter;
int audio_read_number;

//for CDC
char cdc_read_buf[64];
char cdc_write_buf[64];

//--------------------------------------------------------------------+
//functions used in main routine
// void cdc_write_int(int64_t);  //for debugging
void cdc_write(char *, uint16_t);
uint32_t cdc_read(void);

int32_t adc(); 
void freqChange(void);
void receive(void);
void transmit(uint64_t);
void transmitting(void);
void receiving(void);
void audio_data_write(int16_t,int16_t);
void cat(void);
int freqcheck(uint64_t);

//-----------------------------------------------------------------------+
// MAIN  -------------------------------------------------------------+
//-----------------------------------------------------------------------+
int main(void)
{
  board_init();
  stdio_init_all();

  tud_init(BOARD_TUD_RHPORT);
  TU_LOG1("Headset running\r\n");

  //GPIO pin setting ----- 
  gpio_init(pin_A0);
  gpio_set_dir(pin_A0, GPIO_IN); //ADC input pin
  gpio_init(pin_SW);
  gpio_set_dir(pin_SW, GPIO_IN); //SW (freq. change)
  gpio_pull_up(pin_SW);
  gpio_init(pin_RX);
  gpio_set_dir(pin_RX, GPIO_OUT); //RX →　1, TX →　0 (for RX switch)
  gpio_init(pin_TX);
  gpio_set_dir(pin_TX, GPIO_OUT); //TX →　1, RX →　0 (for Driver switch)
  gpio_init(pin_RED);
  gpio_set_dir(pin_RED, GPIO_OUT); //On →　0 (for RED LED)
  gpio_init(pin_GREEN);
  gpio_set_dir(pin_GREEN, GPIO_OUT); //On →　0 (for GREEN LED)
  //gpio_init(pin_BLUE);
  //gpio_set_dir(pin_BLUE, GPIO_OUT); //On →　0 (for GREEN LED)
  gpio_init(pin_LED_POWER);
  gpio_set_dir(pin_LED_POWER, GPIO_OUT); //NEOPIXEL LED POWER

  //NEOPIXEL LED  initialization-----
  gpio_put(pin_LED_POWER, 1);  //NEOPIXEL LED ON
  // todo get free sm
  PIO pio = pio0;
  int sm = 0;
  uint offset = pio_add_program(pio, &ws2812_program);
  ws2812_program_init(pio, (uint)sm, offset, WS2812_PIN, 800000, IS_RGBW);
  put_pixel(urgb_u32(pixel_color[0].data[0], pixel_color[0].data[1], pixel_color[0].data[2]));  //pixel_color[0] = red

  //i2c initialization-----  
  bi_decl(bi_2pins_with_func(I2C_SDA_PIN, I2C_SCL_PIN, GPIO_FUNC_I2C));
  i2c_init(I2C_PORT, 400 * 1000);
  gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
  gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
  gpio_pull_up(I2C_SDA_PIN);
  gpio_pull_up(I2C_SCL_PIN);

  //ADC initialization------ 
  adc_init();
  adc_select_input(0);                        // ADC input pin A0
  adc_run(true);                              // start ADC free running
  adc_set_clkdiv(249.0);                      // 192kHz sampling  (48000 / (249.0 +1) = 192)
  adc_fifo_setup(true,false,0,false,false);   // fifo

  //si5351 initialization-----  
  RF_freq = Freq_table[C_freq];
  si5351_init(I2C_PORT);                            //the port of i2c pins of RP2040
  si5351_set_correction(SI5351_CAL);
  si5351_set_pll(SI5351_PLL_FIXED, SI5351_PLLA);
  si5351_set_freq(RF_freq, SI5351_PLL_FIXED, SI5351_CLK0);  //for TX
  si5351_drive_strength(SI5351_CLK0, SI5351_DRIVE_8MA);
  si5351_clock_enable(SI5351_CLK0, 0);
  si5351_set_freq(RF_freq-(uint64_t)BFO_freq, SI5351_PLL_FIXED, SI5351_CLK1);  //for RX
  si5351_drive_strength(SI5351_CLK1, SI5351_DRIVE_2MA);
  si5351_clock_enable(SI5351_CLK1, 0);
#ifdef Superheterodyne
  si5351_set_freq((uint64_t)BFO_freq, SI5351_PLL_FIXED, SI5351_CLK2);  //for BFO
  si5351_drive_strength(SI5351_CLK2, SI5351_DRIVE_2MA);
  si5351_clock_enable(SI5351_CLK2, 0);
#endif

  // USB Audio initialization (initialization of monodata[])
  for (int i = 0; i < (CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ / 4); i++) {
    monodata[i] = 0;
  }
 
  //transceiver initialization-----(recieving)
  si5351_clock_enable(SI5351_CLK0, 0);   //TX osc. off
  si5351_clock_enable(SI5351_CLK1, 1);   //RX osc. on
#ifdef Superheterodyne
  si5351_clock_enable(SI5351_CLK2, 1);   //BFO osc. on
#endif
  gpio_put(pin_TX, 0);  //TX off
  gpio_put(pin_RX, 1);  //RX on
  gpio_put(pin_RED, ONBOARD_LED_OFF);  //Red LED OFF 
  gpio_put(pin_GREEN, ONBOARD_LED_ON); //Green LED ON
  //gpio_put(pin_BLUE, 1); //BLUE LED OFF

  //read the DC offset value (ADC input)----- 
  sleep_ms(100);
  adc_fifo_drain ();
  adc_offset = adc();

  //watchdog_enable(delay_ms,pause_on_debug);
  watchdog_reboot(0,0,1000);
  board_led_write(ONBOARD_LED_OFF);    

  while (1)
  {
    watchdog_update(); //watchdog
    tud_task(); // TinyUSB device task
    // led_blinking_task();
    cat(); // remote control (simulating Kenwood TS-2000) 
    if (Tx_Start==0) receiving();
    else transmitting();
  }
  return 0;
}

//--------------------------------------------------------------------+
// RADIO
//--------------------------------------------------------------------+
void transmitting(){
  uint64_t audio_freq;
  if (audio_read_number > 0) {
    for (int i=0;i<audio_read_number;i++){
      int16_t mono = monodata[i];
      if ((mono_prev < 0) && (mono >= 0)) {
        if ((mono == 0) && (((double)mono_prev * 1.8 - (double)mono_preprev < 0.0) || ((double)mono_prev * 2.02 - (double)mono_preprev > 0.0))) {    //Detect the sudden drop to zero due to the end of transmission
          Tx_Start = 0;
          break;
        }
        int16_t difference = mono - mono_prev;
        // x=0付近のsin関数をテーラー展開の第1項まで(y=xで近似）
        float delta = (float)mono_prev / (float)difference;
        float period = ((float)1.0 + delta_prev) + (float)sampling - delta;
        audio_freq = (uint64_t)(AUDIOSAMPLING/(double)period); // in Hz    
        if ((audio_freq > 200) && (audio_freq < 3000)){
          cycle_frequency[cycle]=(uint32_t)audio_freq;
          cycle++;
        }
        delta_prev = delta;
        sampling = 0;
        mono_preprev = mono_prev;
        mono_prev = mono;     
      }
      else if ((not_TX_first == 1)  && (mono_preprev == 0) && (mono_prev == 0) && (mono == 0)) {        //Detect non-transmission
        Tx_Start = 0;
        break;
      }
      else {
        sampling++;
        mono_preprev = mono_prev;
        mono_prev = mono;
      }
    }
    if (Tx_Start == 0){
      cycle = 0;
      sampling = 0;
      mono_preprev = 0;
      mono_prev = 0;     
      receive();
      return;
    }
    if ((cycle > 0) && ((board_millis() - Tx_last_mod_time) > 10)){      //inhibit the frequency change faster than 20mS
      audio_freq = 0;
      for (int i = 0;i < cycle;i++){
        audio_freq += cycle_frequency[i];
      }
      audio_freq = audio_freq / (uint64_t)cycle;
      transmit(audio_freq);
      cycle = 0;
      Tx_last_mod_time = board_millis() ;
    }
    not_TX_first = 1;
    Tx_last_time = board_millis() ;
  }
  else if ((board_millis() - Tx_last_time) > 100) {     // If USBaudio data is not received for more than 50 ms during transmission, the system moves to receiving. 
    Tx_Start = 0;
    cycle = 0;
    sampling = 0;
    mono_preprev = 0;
    mono_prev = 0;     
    receive();
    return;
  }
  audio_read_number = USB_Audio_read(monodata);
}

void receiving() {
  audio_read_number = USB_Audio_read(monodata); // read in the USB Audio buffer to check the transmitting
  if (audio_read_number != 0) 
  {
    Tx_Start = 1;
    not_TX_first = 0;
    return;
  }
  freqChange();
  int16_t rx_adc = (int16_t)(adc() - adc_offset); //read ADC data (8kHz sampling)
  // write the same 6 stereo data to PC for 48kHz sampling (up-sampling: 8kHz x 6 = 48 kHz)
  for (int i=0;i<6;i++){
    audio_data_write(rx_adc, rx_adc);
  }
}

void audio_data_write(int16_t left, int16_t right) {
  if (pcCounter >= (48)) {                           //48: audio data number in 1ms
    USB_Audio_write(adc_data, pcCounter);
    pcCounter = 0;  
  }
  adc_data[pcCounter] = (int16_t)((left + right) / 2);
  pcCounter++;
}

void transmit(uint64_t freq){                                //freq in Hz
  if (Tx_Status == 0 && freqcheck(RF_freq+3000)==0)
  {
    gpio_put(pin_RX, 0);   //RX off
    gpio_put(pin_TX, 1);   //TX on
    si5351_clock_enable(SI5351_CLK1, 0);   //RX osc. off
  #ifdef Superheterodyne
    si5351_clock_enable(SI5351_CLK2, 0);   //BFO osc. off
  #endif
    si5351_clock_enable(SI5351_CLK0, 1);   //TX osc. on
    Tx_Status=1;
    gpio_put(pin_RED, ONBOARD_LED_ON);
    gpio_put(pin_GREEN, ONBOARD_LED_OFF);
    adc_run(false);                         //stop ADC free running
  }
  si5351_set_freq((RF_freq + (freq)), SI5351_PLL_FIXED, SI5351_CLK0);
  //RF_freq = RF_freq + freq - freq;
}

void receive(){
  gpio_put(pin_TX,0);  //TX off
  gpio_put(pin_RX,1);  //RX on
  si5351_clock_enable(SI5351_CLK0, 0);   //TX osc. off
  si5351_clock_enable(SI5351_CLK1, 1);   //RX osc. on
#ifdef Superheterodyne
  si5351_clock_enable(SI5351_CLK2, 1);   //BFO osc. on
#endif
  Tx_Status=0;
  gpio_put(pin_RED, ONBOARD_LED_OFF);
  gpio_put(pin_GREEN, ONBOARD_LED_ON);
  // initialization of monodata[]
  for (int i = 0; i < (CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ / 4); i++) {
    monodata[i] = 0;
  } 
  // initialization of ADC and the data write counter
  pcCounter=0;
  adc_fifo_drain ();                     //initialization of adc fifo
  adc_run(true);                         //start ADC free running
}

void freqChange(){
  if (gpio_get(pin_SW)==0 && push_last_time==0){
    push_last_time = board_millis();
  }
  if (gpio_get(pin_SW)==0 && (board_millis() - push_last_time) > 700){     //wait for 700ms long push
    C_freq++;
    if  (C_freq >= N_FREQ){
      C_freq = 0;
    }
    RF_freq = Freq_table[C_freq];
    si5351_set_freq((RF_freq-(uint64_t)BFO_freq), SI5351_PLL_FIXED, SI5351_CLK1);
    put_pixel(urgb_u32(pixel_color[C_freq].data[0], pixel_color[C_freq].data[1], pixel_color[C_freq].data[2]));
    adc_fifo_drain ();
    adc_offset = adc();
    push_last_time = 0;
  }
  if (gpio_get(pin_SW)!=0){
    push_last_time = 0;
  }
}

int32_t adc() {
  int32_t adc = 0;
  for (int i=0;i<24;i++){             // 192kHz/24 = 8kHz
    adc += adc_fifo_get_blocking();   // read from ADC fifo
  }  
  return adc;
}


//remote contol (simulatingKenwood TS-2000)
//original: "ft8qrp_cat11.ico" from https://www.elektronik-labor.de/HF/FT8QRP.html
//with some modifications mainly to be adapted to C language
void cat(void) 
{  
  char receivedPart1[40];
  char receivedPart2[40];    
  char command[3];
  char command2[3];  
  char parameter[38];
  char parameter2[38]; 
  char sent[42];
  char sent2[42];
  
  uint8_t received_length = (uint8_t)cdc_read(); 
  if (received_length == 0) return;
  for (int i = 0;i<received_length;i++){                   //to Upper case
    if('a' <= cdc_read_buf[i] && cdc_read_buf[i] <= 'z'){
        cdc_read_buf[i] = cdc_read_buf[i] - ('a' - 'A');
    } 
    if (cdc_read_buf[i] == '\n') {
      received_length--;              //replace(from "\n" to "")
    }
  }

  char data[64];
  int bufferIndex = 0;
  uint16_t part1_length = 0;
  uint16_t part2_length = 0; 
  
  for (int i = 0; i < received_length; ++i)
  {
    if (cdc_read_buf[i] != ';')
    {
      data[i] = cdc_read_buf[i];
    }
    else
    {
      if (bufferIndex == 0)
      { 
        for(int ii = 0; ii < i; ++ii) {
          receivedPart1[ii] = data[ii];
        }
        part1_length = (uint16_t)i;
        bufferIndex++;
      }
      else
      {  
        for(int ii = part1_length + 1; ii < i; ++ii) {
          receivedPart2[ii-part1_length-1] = data[ii];
        }
        part2_length = (uint16_t)(i - 1)- part1_length; 
        bufferIndex++;
      }
    }
  }

  strncpy(command, receivedPart1, 2); 
  command[2] = '\0'; 
  if (part1_length > 2){
    strncpy(parameter, receivedPart1+2, part1_length - 2); 
    parameter[part1_length - 2] = '\0';
  }
  if (bufferIndex == 2){
    strncpy(command2, receivedPart2, 2);
    command[2] = '\0'; 
    strncpy(parameter2, receivedPart2+2, part2_length -2);
    parameter2[part2_length - 2] = '\0';
  }

  if (strcmp(command,"FA")==0){          
    if (sizeof(part1_length) <= 2)
    {          
      long int freqset = strtol(parameter, NULL, 10);
      if (freqset >= 1000000 && freqset <= 54000000){
        RF_freq = (uint64_t)freqset;
        si5351_set_freq((RF_freq-(uint64_t)BFO_freq), SI5351_PLL_FIXED, SI5351_CLK1);
        si5351_set_freq((RF_freq), SI5351_PLL_FIXED, SI5351_CLK0);
        //NEOPIXEL LED change
        put_pixel(urgb_u32(pixel_color[7].data[0], pixel_color[7].data[1], pixel_color[7].data[2]));
        adc_fifo_drain ();
        adc_offset = adc();
      }
    }
    strcpy(sent, "FA"); // Return 11 digit frequency in Hz.
    snprintf(parameter, 12, "%011d", (int)RF_freq);
    strcat(sent,parameter); 
    strcat(sent, ";");
  }
  else if (strcmp(command,"FB")==0) {                   
    strcpy(sent, "FB"); // Return 11 digit frequency in Hz.
    snprintf(parameter, 12, "%011d", (int)RF_freq);
    strcat(sent,parameter); 
    strcat(sent, ";");
  }
  else if (strcmp(command,"IF")==0) {          
    strcpy(sent, "IF"); // Return 11 digit frequency in Hz.  
    snprintf(parameter, 12, "%011d", (int)RF_freq);
    strcat(sent, parameter);
    strcat(sent, "0001+0000000000"); 
    snprintf(parameter, 2, "%d", Tx_Status);
    strcat(sent, parameter);  
    strcat(sent, "20000000;");          //USB   
  }
  else if (strcmp(command,"MD")==0) {
    strcpy(sent, "MD2");                //USB
  }
  else  if (strcmp(command,"ID")==0)  {  
    strcpy(sent,"ID019;");
  }
  else  if (strcmp(command,"PS")==0)  {  
    strcpy(sent, "PS1;");
  }
  else  if (strcmp(command,"AI")==0)  {  
    strcpy(sent, "AI0;");
  }
  else  if (strcmp(command,"RX")==0)  {  
    strcpy(sent, "RX0;");
  }
  else  if (strcmp(command,"TX")==0)  {  
    strcpy(sent, "TX0;");
  }
  else  if (strcmp(command,"AG")==0)  {  
    strcpy(sent, "AG0000;");
  }
  else  if (strcmp(command,"XT")==0) {  
    strcpy(sent, "XT0;");
  }
  else  if (strcmp(command,"RT")==0)  {  
    strcpy(sent, "RT0;");
  }
  else  if (strcmp(command,"RC")==0)  {  
    strcpy(sent, ";");
  }
  else  if (strcmp(command,"RS")==0)  {  
    strcpy(sent, "RS0;");
  }
  else  if (strcmp(command,"VX")==0)  {  
    strcpy(sent, "VX0;"); 
  }
  else  {
    strcpy(sent, "?;"); 
  }
//----------------------------------------------------
  if (strcmp(command2,"ID")==0)   {  
    strcpy(sent2, "ID019;");
  }
  else  {
    strcpy(sent2, "?;"); 
  }               
  
  if (bufferIndex == 2)  {
    cdc_write(sent2, (uint16_t)strlen(sent2));
  }        
  else  {
    cdc_write(sent, (uint16_t)strlen(sent));
  }  
}

//--------------------------------------------------------------------+
// USB CDC functions 
/*
//for debugging
void cdc_write_int(int64_t integer) 
{
  char buf[64];
  int length = sprintf(buf, "%lld", integer);
  tud_cdc_write(buf, (uint32_t)length);
  tud_cdc_write_flush();
}
*/

void cdc_write(char *buf, uint16_t length)
{
  tud_cdc_write(buf, length);
  tud_cdc_write_flush();
}

uint32_t cdc_read(void)
{
  if ( tud_cdc_available() )
  {
    // read data
    uint32_t count = tud_cdc_read(cdc_read_buf, sizeof(cdc_read_buf));
    (void) count;
      return count;
  }
  else{
    return 0;
  }
}

//--------------------------------------------------------------------+
//Out-of-band transmission prevention check (in JA)
//This routine is required to obtain a license in Japan.
int freqcheck(uint64_t frequency)  // retern 1=out-of-band, 0=in-band
{
  if (frequency < 135700) {
    return 1;
  }
  else if (frequency > 135800 && frequency < 472000) {
    return 1;
  }
  else if (frequency > 479000 && frequency < 1800000) {
    return 1;
  }
  else if (frequency > 1875000 && frequency < 1907500) {
    return 1;
  }
  else if (frequency > 1912500 && frequency < 3500000) {
    return 1;
  }
  else if (frequency > 3580000 && frequency < 3662000) {
    return 1;
  }
  else if (frequency > 3687000 && frequency < 3716000) {
    return 1;
  }
  else if (frequency > 3770000 && frequency < 3791000) {
    return 1;
  }
  else if (frequency > 3805000 && frequency < 7000000) {
    return 1;
  }
  else if (frequency > 7200000 && frequency < 10100000) {
    return 1;
  }
  else if (frequency > 10150000 && frequency < 14000000) {
    return 1;
  }
  else if (frequency > 14350000 && frequency < 18068000) {
    return 1;
  }
  else if (frequency > 18168000 && frequency < 21000000) {
    return 1;
  }
  else if (frequency > 21450000 && frequency < 24890000) {
    return 1;
  }
  else if (frequency > 24990000 && frequency < 28000000) {
    return 1;
  }
  else if (frequency > 29700000 && frequency < 50000000) {
    return 1;
  }
  else if (frequency > 54000000) {
    return 1;
  }
  else return 0;
}
