/**
 * MX5 NBFL Immobilizer Bypass for Digispark ATtiny85 USB Mini Development Board.
 *
 * Compile:
 *	avr-gcc -Wall -Os -DF_CPU=16000000 -mmcu=attiny85 -o main.elf main.c
 *	avr-objcopy -j .text -j .data -O ihex main.elf main.hex
 *
 * Programming - USB Mini Development Board:
 *	./micronucleus --run main.hex
 *
 * Programming - ATtiny85 with the parallel port:
 *  avrdude -c dapa -p t85 -U lfuse:w:0xf1:m -U flash:w:main.hex:i
 *
 * Read EEPROM (Code):
 *  avrdude -c dapa -p t85 -U eeprom:r:eedump.hex:i
 */

#include <avr/io.h>
#include <util/delay.h>
#include <avr/interrupt.h>
#include <avr/eeprom.h>
#include <stdint.h>

/**
 * LED Port:
 * Blink 2s - Startup
 * Blink 100ms - Simulate
 * Blink 1s - Learn
 *
 *               R3
 * LED1_PIN ---/\/\/\--- LED --- GND
 *
 * R3 = 220ohm
 *
 */
#define LED1_PIN PB1

/**
 * DATA Line
 * Bus = Line between Immobilizer and PCM (Red/Black wire).
 * 
 *  Bus            RX_INV_PIN
 *   |                | c
 *   |      R1       /
 *   +----/\/\/\----| b  Q1
 *   |               \
 *   |                | e
 *   | c             GND
 *    \         R2
 * Q2  | b ---/\/\/\--- TX_INV_PIN
 *    /
 *   | e
 *  GND
 *
 * R1 = 150kohm
 * R2 = 1.5kohm
 * Q1/2 = Any NPN Transistor (BC547)
 * 
 * It's a bus with 1200 bits/s, 1 start bit followed with 8 data bit.
 * Need 7.5 mA to pull down the bus. (40 mA on ATtiny)
 */
#define RX_INV_PIN PB2
#define TX_INV_PIN PB0

/**
 * When chip goes on, PB4 is on for 2 seconds.
 * Good to simulate cluster lamp check.
 */
#define CLUSTER_PIN PB4

/**
 * States:
 *	s_idle:
 *		Initial state waiting for the PCM.
 *	s_waitimm:
 *		Wait a little to see if the original Immobilizer answer the request.
 *		If there is an Immobilizer jump in s_learn otherwise in s_simulate.
 *	s_learn:
 *		Learn the code from the Immobilzer and save it into EEPROM.
 *	s_simulate:
 *		Replay the learned code.
 */
enum state { s_idle, s_waitimm, s_learn, s_simulate };
enum state g_state = s_idle;

/** What's is going on the bus? */
#define is_reading_byte() (g_readed_bits != 0xFFFF)
#define is_writing_byte() (g_state == s_simulate)

/** Global variables */
#define IMM_CODE_LEN 5
uint8_t g_imm_code_learn[IMM_CODE_LEN];
uint8_t g_imm_code[IMM_CODE_LEN]; // = {0xC2, 0xA2, 0x1A, 0x88, 0xCE};
uint8_t g_imm_answer_timeout;
uint8_t g_imm_learn_timeout;
uint16_t g_led = 2400; /* At startup leave the LED for 2 sec. */
uint16_t g_readed_bits = 0xFFFF;

/** Goooo! */
int main(void)
{
	uint8_t dataIn[2] = {1,1};
	unsigned int i;

	/* Disable global interrupts */
	cli();

	/* Timer 1 */
	TCCR1 = 0x00; /* Stop */
	OCR1A = 80;   /* Counter A - 75 % of 52 for sampling */ 
	OCR1B = 105;   /* Counter B - 62.5 kHz / 52 = ~1200 Hz */ 
	TIMSK = (1 << OCIE1A) | (1 << OCIE1B); /* Enable compare A/B interrupt */
	TCCR1 = 0x08; /* Start, divides by 256 - 16MHz / 128 = 62.5 kHz */

	/* Enable global interrupts */
	sei();

	/* Set output for LED, TX Pin and Cluster*/
	DDRB = (1 << LED1_PIN) | (1 << TX_INV_PIN) | (1 << CLUSTER_PIN);

	/* Set Pull-Up for RX Pin. */
	PORTB = (1 << RX_INV_PIN);

	/* Read the needed code from EEPROM */
	for(i=0; i<IMM_CODE_LEN; ++i)
		g_imm_code[i] = eeprom_read_byte((uint8_t*)i);

	while(1){
		dataIn[1] = dataIn[0];
		dataIn[0] = (PINB >> RX_INV_PIN) & 1;

		/* Timer 1 - Synchronize (Reset Counter) on falling/raising edge. */
		if(dataIn[1] != dataIn[0] && !is_writing_byte()) TCNT1 = 0;
	}
}

/**
 * Called at every received Byte.
 */
void handle_recv_byte(uint8_t byte) {
	static uint8_t cur_byte;
	unsigned int i;

	switch(g_state){		
		/* PCM ask the immobilizer */
		case s_idle:
			/* TODO: It is always 0x0E ??? - I guess so. */
			/* Document CT-L1007_en has a screenshot where it's also a 0x0E. */
			if(byte == 0x0E){
				/* Set LED on for 100ms */
				g_led = 120;

				/* See if the Immobilzer answer within 5x(1/1200) seconds */				
				cur_byte = 0;
				g_imm_answer_timeout = 5;
				g_state = s_waitimm;
			}
			break;
		/* Sould we learn ? */
		case s_learn:
			g_imm_code_learn[cur_byte++] = byte;
			if(cur_byte >= IMM_CODE_LEN){
				/* Set LED on for 1s */
				g_led = 1200;
				g_state = s_idle;

				/* Write the code to EEPROM */
				for(i=0; i<IMM_CODE_LEN; ++i){
					if(g_imm_code[i] != g_imm_code_learn[i]){
						g_imm_code[i] = g_imm_code_learn[i];
						eeprom_write_byte((uint8_t*)i, g_imm_code[i]);
					}
				}
			}
			break;
		default:
			break;
	}
}

/**
 * Interrupt called at 75% of a bit signal. Ideal for sampling.
 * Call handle_recv_byte() after reading a Start-Bit und 8 Data Bits.
 */
ISR(TIMER1_COMPA_vect) {
	if(is_writing_byte()) return;

	g_readed_bits = (g_readed_bits << 1) | ((~PINB >> RX_INV_PIN) & 1);

	/* Oh, bit 9 (Start Bit) is 0, handle the received byte */
	if((g_readed_bits & 0x0100) == 0) {
		handle_recv_byte((uint8_t)g_readed_bits & 0xFF);
		g_readed_bits = 0xFFFF; /* Reset for next byte */
	}
}

/**
 * Interrupt called at the end of a bit signal. Ideal for writing the next bit.
 */
ISR(TIMER1_COMPB_vect) {
	static uint8_t cur_byte = 0, cur_bit = 9;
	static uint16_t clusterlight = 2400;

	/* Timer 1 - Reset counter */
	TCNT1 = 0;

	/* LED */
	if(g_led > 0){ --g_led; PORTB |=  (1 << LED1_PIN); } /* LED on */
	else PORTB &= ~(1 << LED1_PIN); /* LED off */

	/* Cluster LAMP check simulation. */
	if(clusterlight > 0){ --clusterlight; PORTB |=  (1 << CLUSTER_PIN); }
	else PORTB &= ~(1 << CLUSTER_PIN);

	switch(g_state){
		/* Decrement for Immobilizer answer timeout */
		case s_waitimm:
			if(g_imm_answer_timeout > 0){
				/* There is an Immobilizer Unit on the Bus */
				if(is_reading_byte()){
					g_imm_learn_timeout = 176;
					g_state = s_learn;
				}else --g_imm_answer_timeout;
			}else g_state = s_simulate;
			break;
		/* Sould we learn ? */
		case s_learn:
			if(g_imm_learn_timeout > 0){
				--g_imm_learn_timeout;
			}else g_state = s_idle;
			break;
		/* Write output */
		case s_simulate:
			if(cur_bit > 9 || cur_bit == 0)   /* Idle or Space */ 
				PORTB &= ~(1 << TX_INV_PIN);
			else if(cur_bit == 9)             /* Start bit */
				PORTB |=  (1 << TX_INV_PIN);  
			else{                             /* Data Bit */					
				if(g_imm_code[cur_byte] & (1 << (cur_bit-1)))
					PORTB &= ~(1 << TX_INV_PIN); /* Logic Data 1 */
				else
					PORTB |=  (1 << TX_INV_PIN); /* Logic Data 0 */
			}

			/* Go to the next bit. */
			if (cur_bit > 0) --cur_bit;
			else{
				cur_bit = 18; /* 18-10 Space, 9 Start, 8-1 Data, 0 Idle */
				++cur_byte;
				if(cur_byte >= IMM_CODE_LEN){
					/* No space before the first byte, we have waited enough
					   with g_imm_answer_timeout. */
					cur_bit = 9;
					cur_byte = 0;
					g_state = s_idle;
				}
			}
			break;
		default:
			break;
	}
}


