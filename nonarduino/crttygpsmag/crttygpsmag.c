/* Arduino GPS Parser and RTTY Modulator */

#define USART_BAUDRATE 9600
#define BAUD_PRESCALE (((F_CPU / (USART_BAUDRATE *16UL)))-1)

#include <stdlib.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <string.h>
#include "ita2.h"
#include "pwmsine.h"

/* Magnetometer Chip Stuff */
uint8_t lsm303_mag_addr_w = 0x3c;
uint8_t lsm303_mag_addr_r = 0x3d;
uint8_t lsm303_mag_xh = 0x03;

static unsigned int sampleRate = 60000; /*62500; */
static unsigned int tableSize = 1024; /* (unsigned int)(sizeof(sine)/sizeof(char)); */
static unsigned int pstn = 0;
static signed char sign = (signed char)-1;
static unsigned int change = 0;
static unsigned int count = 1;

/* Different baudrates and shifts tested to work */
/* 45bps - 870, 700, 45 */
/* 50bps - 785, 700, 50 */
/* 50bps - 870, 700, 50 */
/* 75bps - 870, 700, 75 */
/* 110bps - 1050, 700, 110 */

static unsigned int fmark = 870;
static unsigned int fspac = 700;
static unsigned int baud = 45;
static unsigned int bits = 8;
static unsigned char lsbf = (unsigned char)0;

static const unsigned char maxmsg = (unsigned char)128;
static char msg[128] = "";

static unsigned char bitPstn = (unsigned char)0;
static unsigned int bytePstn = (unsigned char)0;
static unsigned char tx = (unsigned char)0;

static unsigned char charbuf = (unsigned char)0;
static unsigned char shiftToNum = (unsigned char)0;
static unsigned char justshifted = (unsigned char)0;

/* Compute values for tones. */
/*
 * static unsigned int dmark = (unsigned int)((2*(long)tableSize*(long)fmark)/((long)sampleRate));
 * static unsigned int dspac = (unsigned int)((2*(long)tableSize*(long)fspac)/((long)sampleRate));
 */
static unsigned int dmark = (unsigned int)0;
static unsigned int dspac = (unsigned int)0;
static unsigned char msgSize = (unsigned char)0;
static unsigned int sampPerSymb = (unsigned int)0; /*(unsigned int)(sampleRate/baud);*/

static char delim[] = ":";
static char nl[] = "\n";
static char call[] = "KD8ZRC";
static char nulls[] = "R1R1R1R1";

static char lat_deg = (char)0;
static char lon_deg = (char)0;
static float lat_f = 0;
static float lon_f = 0;

/* GPS Data */
static const char buflen = (char)16;
static char utc_time[16] = "";
static char longitude[16] = "";
static char NS[1] = "";
static char latitude[16] = "";
static char EW[1] = "";
static char altitude[16] = "";
static char alt_units[1] = "";
static char strbuf[2] = "";

/* Intialize Finite fsm_state Machine */
static char buffer[16]; 
static char fsm_state = (char)0;
static char commas = (char)0;
static char lastcomma = (char)0;

static uint8_t twit_start_init = 0x08;
static uint8_t twit_start_rp   = 0x10;
static uint8_t twit_addr_ack   = 0x18;
/*static uint8_t twit_addr_nack  = 0x20;*/
static uint8_t twit_data_ack   = 0x28;
/*static uint8_t twit_data_nack  = 0x30;*/
/*static uint8_t twit_e = 0x38;*/

static uint8_t twir_addr_ack  = 0x40;
/*static uint8_t twir_addr_nack = 0x48;*/
static uint8_t twir_data_ack  = 0x50;
static uint8_t twir_data_nack = 0x58;

static char cmag_x[10] = "";
static char cmag_y[10] = "";
static char cmag_z[10] = "";

static int16_t mag_x = 0;
static int16_t mag_y = 0;
static int16_t mag_z = 0;


static void twi_init(void)
{
  /* set clock to 400kHz */
  TWSR = 0x00;
  TWBR = 0x0c;
  /* enable twi */
  TWCR = (1<<TWEN);
}

static void twi_start(void)
{
  TWCR = (1<<TWINT) | (1<<TWSTA) | (1<<TWEN);
  while ((TWCR & (1<<TWINT)) == 0);
}

static void twi_stop(void)
{
  TWCR = (1<<TWINT) | (1<<TWSTO) | (1<<TWEN);
}

static void twi_write(uint8_t bsend)
{
  TWDR = bsend;
  TWCR = (1<<TWINT) | (1<<TWEN);
  while ((TWCR & (1<<TWINT)) == 0);
}

static uint8_t twi_read_ack(void)
{
  TWCR = (1<<TWINT) | (1<<TWEN) | (1<<TWEA);
  while ((TWCR & (1<<TWINT)) == 0);
  return TWDR;
}

static uint8_t twi_read_nack(void)
{
  TWCR = (1<<TWINT) | (1<<TWEN);
  while ((TWCR & (1<<TWINT)) == 0);
  return TWDR;
}

static uint8_t twi_status(void)
{
  uint8_t stat;
  stat = TWSR & 0xf8;
  return stat;
}

static uint8_t read_mag()
{
  twi_start();
  if (twi_status() != twit_start_init) return 1;  
  
  twi_write(lsm303_mag_addr_w);
  if (twi_status() != twit_addr_ack) return 1;

  twi_write(lsm303_mag_xh);
  if (twi_status() != twit_data_ack) return 1;

  twi_start();
  if (twi_status() != twit_start_rp) return 1;

  twi_write(lsm303_mag_addr_r);
  if (twi_status() != twir_addr_ack) return 1;

  uint8_t xh = twi_read_ack();
  if (twi_status() != twir_data_ack) return 1;

  uint8_t xl = twi_read_ack();
  if (twi_status() != twir_data_ack) return 1;

  uint8_t zh = twi_read_ack();
  if (twi_status() != twir_data_ack) return 1;

  uint8_t zl = twi_read_ack();
  if (twi_status() != twir_data_ack) return 1;

  uint8_t yh = twi_read_ack();
  if (twi_status() != twir_data_ack) return 1;

  uint8_t yl = twi_read_nack();
  if (twi_status() != twir_data_nack) return 1;

  twi_stop();

  mag_x = (int16_t)(xl | ((int16_t)xh<<8));
  mag_y = (int16_t)(yl | ((int16_t)yh<<8));
  mag_z = (int16_t)(zl | ((int16_t)zh<<8));

  return 0;
}









static void init()
{
  dmark = (unsigned int)((2*(long)tableSize*(long)fmark)/((long)sampleRate));
  dspac = (unsigned int)((2*(long)tableSize*(long)fspac)/((long)sampleRate));
  tableSize = (unsigned int)(sizeof(sine)/sizeof(char)); 
  sampPerSymb = (unsigned int)(sampleRate/baud);
}

static unsigned char calcAmp()
{
  pstn += change;

  /* if the position rolls off, change sign and start where you left off */
  if(pstn >= tableSize) {
    pstn = pstn >> 10; /* (mod 1024) */
    sign = (signed char)-sign;
  }

  /* return the pwm value */
  return (unsigned char)(128+(int)(sign*sine[pstn]));
}




/* sets the character buffer, the current character being sent */
static void setCbuff()
{
    unsigned int i = 0;

    /*  Note: the <<2)+3 is how we put the start and stop bits in
     * the ita2 table is MSB on right in the form 000xxxxx
     * so when we shift two and add three it becomes 0xxxxx11 which is
     * exactly the form we need for one start bit and two stop bits when read
     * from left to right
     */

    /* try to find a match of the current character in ita2 */
    for(i = 0; i < (unsigned int)(sizeof(ita2_letters)/sizeof(char)); i++){

      /* look in letters */
      if((unsigned char)msg[bytePstn] == ita2_letters[i]) {

        /* if coming from numbers, send shift to letters */
        if(shiftToNum == (unsigned char)1){
          shiftToNum = (unsigned char)0;
          charbuf = (unsigned char)(((ita2[31])<<2)+(unsigned char)3);
          justshifted = (unsigned char)1;
        } else {
          charbuf = (unsigned char)(((ita2[i])<<2)+(unsigned char)3);
        }
      }

      /* look in numbers */
      if(msg[bytePstn] != ' ' && msg[bytePstn] != (char)10
		&& (unsigned char)msg[bytePstn] == ita2_figures[i]) {
        if(shiftToNum == (unsigned char)0){
          shiftToNum = (unsigned char)1;
          charbuf = (unsigned char)(((ita2[30])<<2)+(unsigned char)3);
          justshifted = (unsigned char)1;
        } else {
          charbuf = (unsigned char)(((ita2[i])<<2)+(unsigned char)3);
        }
      }      
      

    }
    
  /* dont increment bytePstn if were transmitting a shift-to character */
  if(justshifted != (unsigned char)1) {
    /* print letter you're transmitting */
    UDR0 = (unsigned char)msg[bytePstn];
    bytePstn++;
  } else {
    justshifted = (unsigned char)0;
  }
}




static void setSymb(unsigned char hmve)
{

    /* if its a 1 set change to dmark otherwise set change to dspace */
    if( ( ( (unsigned char)charbuf & ( 0x01<<hmve ) ) >> hmve ) == (unsigned char)1 ) {
      change = dmark;
    } else {
      change = dspac;
    }
}



int main(void)
{

  /* set things up */
  init();
  twi_init();

  /* setup pins for output */
  DDRD |= _BV(3);

  /* setup counter 2 for fast PWM output on pin 3 (arduino) */
  TCCR2A = _BV(COM2B1) | _BV(WGM21) | _BV(WGM20);
  TCCR2B = _BV(CS20); /* TinyDuino at 8MHz */
  /*TCCR2B = _BV(CS21);*/  /* Arduino at 16MHz */
  TIMSK2 = _BV(TOIE2);
  
  /* begin serial communication */
  UCSR0B = (1 << RXEN0) | (1 << TXEN0);
  UCSR0C = (1 << UCSZ00) | (1 << UCSZ01);
  UBRR0H = (BAUD_PRESCALE >> 8);
  UBRR0L = BAUD_PRESCALE;
  UCSR0B |= (1 << RXCIE0);
  UDR0 = 'S';

  /* re-enable interrupts */
  sei();

  /* Does NOT Terminate! */
  for(;;) {
    /* Idle here while interrupts take care of things */
  }
}




/*
 * This interrupt run on every sample (7128.5 times a second)
 * though it should be every 7128.5 the sample rate had to be set differently
 * to produce the correct baud rate and frequencies (that agree with the math)
 * Tune your device accordingly!
 */

/*@ -emptyret @*/
ISR(/*@ unused @*/ TIMER2_OVF_vect) {
  unsigned char hmve = (unsigned char)0;

  if (tx == (unsigned char)0) return;
  count--;

  /* if we've played enough samples for the symbol were transmitting, get next symbol */
  if (count == (unsigned int)0){
    bitPstn = (unsigned char)(bitPstn+(unsigned char)1);
    count = sampPerSymb;
    
    /* Next commented out for 2 stop bits */
    /* if were transmitting the last stop bit make it 0.5x as long (1.5 stop bits)
    if (bitPstn == (unsigned char)(bits-1)) count = count >> 1;  div by 2 */

    /* if were at the end of the character return to the begining of a
     * character and grab the next one */

    if(bitPstn > (unsigned char)(bits-1)) {
      bitPstn = (unsigned char)0;
      
      setCbuff();
      /* if were at the end of the message, return to the begining */
      if (bytePstn >= (unsigned int)msgSize){
        /* clear variables used here */
        msgSize = (unsigned char)0;
        bitPstn = (unsigned char)0;
        bytePstn = (unsigned char)0;
        count = (unsigned int)1;
        tx = (unsigned char)0;
        return;
      }
    }

    /* endianness thing */
    if (lsbf != (unsigned char)1) {
      /* MSB first */
      hmve = (unsigned char)((bits-1)-(int)bitPstn);
    } else {
      /* LSB first */
      hmve = bitPstn;
    }

    /* get if 0 or 1 that were transmitting */
    setSymb(hmve);
  }

  /* set PWM duty cycle */
  OCR2B = calcAmp();
  return;
}

ISR(/*@ unused @*/ USART_RX_vect) {
  int i;

  if (tx == (unsigned char)1) return;
 
  /* move buffer down, make way for new char */
  for(i = 0; i < (int)buflen-1; i++){
    buffer[i] = buffer[i+1];      
  }

  buffer[(int)buflen-1] = (char)UDR0;

  if(buffer[(int)buflen-1] == ',') commas = (char)(commas + (char)1);

  /* reset FSM on new data frame */
  if(buffer[(int)buflen-1] == '$') {
    commas = (char)0;
    fsm_state = (char)0;
  }

  /* Finite fsm_state Machine */
  if (fsm_state == (char)0) {

    if (buffer[(int)buflen-6] == '$' &&
        buffer[(int)buflen-5] == 'G' &&
        buffer[(int)buflen-4] == 'P' &&
        buffer[(int)buflen-3] == 'G' &&
        buffer[(int)buflen-2] == 'G' &&
        buffer[(int)buflen-1] == 'A'){
      fsm_state = (char)1;
    }

  } else if (fsm_state == (char)1) {

    /* Grab time info
     * If this is the second comma in the last element of the buffer array */
    if (buffer[(int)buflen-1] == ',' && commas == (char)2){
      int j;
      utc_time[0] = '\0';
      for (j = (int)lastcomma; j < (int)buflen-1; j++) {

        strbuf[0] = buffer[j];
        strncat(utc_time, strbuf, (size_t)(buflen-(char)1)-(unsigned int)j);
      }

      /* next fsm_state */
      fsm_state = (char)2;
    }

  } else if (fsm_state == (char)2) { 

    /* Grab latitude */

    if (buffer[(int)buflen-1] == ',' && commas == (char)3){
      int j;
      latitude[0] = '\0';
      for (j = (int)lastcomma; j < (int)buflen-1; j++) {
        strbuf[0] = buffer[j];
        strncat(latitude, strbuf, (size_t)(buflen-(char)1)-(unsigned int)j);
      }

      fsm_state = (char)3;
    }

  } else if (fsm_state == (char)3) {

    /* Grab N or S reading */
    if (buffer[(int)buflen-1] == ',' && commas == (char)4){
      strncpy(NS, &buffer[(int)buflen-2], 1);
      fsm_state = (char)4;
    }

  } else if (fsm_state == (char)4) {

    /* Grab longitude */
    if (buffer[(int)buflen-1] == ',' && commas == (char)5){
      int j;
      longitude[0] = '\0';
      for (j = (int)lastcomma; j < (int)buflen-1; j++) {
        strbuf[0] = buffer[j];
        strncat(longitude, strbuf, (size_t)(buflen-(char)1)-(unsigned int)j);
      }

      fsm_state = (char)5;
    }

  } else if (fsm_state == (char)5) {

    /* Grab E or W reading */
    if (buffer[(int)buflen-1] == ',' && commas == (char)6){
      strncpy(EW, &buffer[(int)buflen-2], 1);
      fsm_state = (char)6;
    }

  } else if (fsm_state == (char)6) {

    /* Grab altitude */
    if (buffer[(int)buflen-1] == ',' && commas == (char)10){
      int j;
      altitude[0] = '\0';
      for (j = (int)lastcomma; j < (int)buflen-1; j++) {
        strbuf[0] = buffer[j];
        strncat(altitude, strbuf, (size_t)(buflen-(char)1)-(unsigned int)j);
      }

      fsm_state = (char)7;
    }

  } else if (fsm_state == (char)7) {

    /* Grab altitude units */
    if (buffer[(int)buflen-1] == ',' && commas == (char)11){
      strncpy(alt_units, &buffer[(int)buflen-2], 1);
      fsm_state = (char)8;
    }

  } else if (fsm_state == (char)8) {
    
    /* clear lat and lon */
    lat_deg = (char)0;
    lon_deg = (char)0;
    lat_f = (char)0;
    lon_f = (char)0;

    /* convert lat and lon from deg decimal-minutes, to decimal degrees */
    lat_deg = (char)( ((latitude[0]-(char)48)*(char)10) + (latitude[1]-(char)48));
    lat_f = (float)lat_deg + ((float)atof(&latitude[2]))/60;
    lon_deg = (char)( ((longitude[0]-(char)48)*(char)100)+((longitude[1]-(char)48)*(char)10)+(longitude[2]-(char)48) );
    lon_f = (float)lon_deg + ((float)atof(&longitude[3]))/60;

    /* make negative if needed */
    if (NS[0] == 'S') lat_f = -lat_f;     
    if (EW[0] == 'W') lon_f = -lon_f;   
    
    /* convert back to strings */
    dtostrf(lat_f, 8, 5, latitude);
    dtostrf(lon_f, 8, 5, longitude);        
    
    mag_x = 0;
    mag_y = 0;
    mag_z = 0;

    if(read_mag() != 0) {
     cmag_x[1] = 'E';
     cmag_x[2] = '\0';
    } else {
      itoa(mag_x, cmag_x, 10);
      itoa(mag_y, cmag_y, 10);
      itoa(mag_z, cmag_z, 10);
    }

    /* assemble the message */
    strncpy(msg, nulls,     (size_t)maxmsg);
    strncat(msg, nl,        (size_t)maxmsg);
    strncat(msg, delim,     (size_t)maxmsg);
    strncat(msg, call,      (size_t)maxmsg);
    strncat(msg, delim,     (size_t)maxmsg);
    strncat(msg, latitude,  (size_t)maxmsg);
    strncat(msg, delim,     (size_t)maxmsg);
    strncat(msg, longitude, (size_t)maxmsg);
    strncat(msg, delim,     (size_t)maxmsg);
    strncat(msg, altitude,  (size_t)maxmsg);
    strncat(msg, delim,     (size_t)maxmsg);
    strncat(msg, utc_time,  (size_t)6);
    strncat(msg, delim,     (size_t)maxmsg);
    strncat(msg, cmag_x,    (size_t)6);
    strncat(msg, delim,     (size_t)maxmsg);
    strncat(msg, cmag_y,    (size_t)6);
    strncat(msg, delim,     (size_t)maxmsg);
    strncat(msg, cmag_z,    (size_t)6);
    strncat(msg, delim,     (size_t)maxmsg);
    strncat(msg, nl,        (size_t)maxmsg);
    strncat(msg, nl,        (size_t)maxmsg);
    msgSize = (unsigned char)strlen(msg);

    /* reset finite state machine and transmit... */
    commas = (char)0;
    fsm_state = (char)0;
    tx = (unsigned char)1;
  }
  
  if(buffer[(int)buflen-1] == ',') {
    lastcomma = (char)((int)buflen-1);
  } else {
    lastcomma = (char)(lastcomma - (char)1);
  }

  return;
}
