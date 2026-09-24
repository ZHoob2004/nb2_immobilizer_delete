/**
 * MX5 NBFL Immobilizer Bypass for a bare ATtiny85.
 *
 * Target configuration:
 *   - F_CPU: 1 MHz (8 MHz internal RC oscillator with CKDIV8 enabled)
 *   - TX_INV: PB3
 *   - RX_INV: PB4, with an external 10 kOhm pull-up to VCC
 *   - PB0/PB1/PB2: reserved for ISP only
 */

#ifndef F_CPU
#define F_CPU 1000000UL
#endif

#if F_CPU != 1000000UL
#error "This firmware is configured for a 1 MHz ATtiny85 system clock."
#endif

#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/eeprom.h>
#include <stdint.h>

#define TX_INV_PIN PB3
#define RX_INV_PIN PB4

#define IMM_CODE_LEN 5

/*
 * Timer1 runs from the 1 MHz system clock divided by 8, giving a 125 kHz
 * timer clock. OCR1A/OCR1B intentionally retain the timing of the original
 * 16 MHz firmware, which used a /128 Timer1 prescaler and therefore the
 * same 125 kHz timer clock.
 */
#define TIMER1_COMPARE_A 80
#define TIMER1_COMPARE_B 105

enum state {
    s_idle,
    s_waitimm,
    s_learn,
    s_simulate
};

static volatile enum state g_state = s_idle;

#define is_reading_byte() (g_readed_bits != 0xFFFF)
#define is_writing_byte() (g_state == s_simulate)

static uint8_t g_imm_code_learn[IMM_CODE_LEN];
static volatile uint8_t g_imm_code[IMM_CODE_LEN];
static volatile uint8_t g_imm_answer_timeout;
static volatile uint8_t g_imm_learn_timeout;
static volatile uint16_t g_readed_bits = 0xFFFF;

static void handle_recv_byte(uint8_t byte);

int main(void)
{
    uint8_t data_in[2] = {1, 1};
    unsigned int i;

    cli();

    /*
     * PB3 is the only application output. Keep TX low so Q2 is off.
     * PB4 remains an input and uses the external 10 kOhm pull-up.
     * PB0/PB1/PB2 are unused during normal operation; enable their internal
     * pull-ups so the exposed ISP pads do not float when no programmer is
     * attached. During reset/ISP, the programming interface takes control.
     */
    DDRB = (1 << TX_INV_PIN);
    PORTB = (1 << PB0) | (1 << PB1) | (1 << PB2);

    /* Read the learned immobilizer code from EEPROM before enabling IRQs. */
    for (i = 0; i < IMM_CODE_LEN; ++i)
        g_imm_code[i] = eeprom_read_byte((uint8_t *)i);

    /*
     * Timer1:
     *   F_CPU = 1 MHz
     *   prescaler = 8 (CS12 = 1)
     *   timer clock = 125 kHz
     *
     * Compare A is used for receive sampling and compare B advances the
     * transmit/state-machine timing. Compare B ISR resets TCNT1 each bit.
     */
    TCCR1 = 0x00;
    TCNT1 = 0;
    OCR1A = TIMER1_COMPARE_A;
    OCR1B = TIMER1_COMPARE_B;

    /* Clear any pending Timer1 compare flags before enabling interrupts. */
    TIFR = (1 << OCF1A) | (1 << OCF1B);
    TIMSK = (1 << OCIE1A) | (1 << OCIE1B);

    /* Synchronous Timer1 clock, CK/8. */
    TCCR1 = (1 << CS12);

    sei();

    for (;;) {
        data_in[1] = data_in[0];
        data_in[0] = (PINB >> RX_INV_PIN) & 1;

        /* Synchronize Timer1 to either edge while not transmitting. */
        if (data_in[1] != data_in[0] && !is_writing_byte())
            TCNT1 = 0;
    }
}

/* Called after a complete byte is received from the bus. */
static void handle_recv_byte(uint8_t byte)
{
    static uint8_t cur_byte;
    unsigned int i;

    switch (g_state) {
    case s_idle:
        /* PCM immobilizer request. */
        if (byte == 0x0E) {
            /* Wait briefly to see whether the original immobilizer answers. */
            cur_byte = 0;
            g_imm_answer_timeout = 5;
            g_state = s_waitimm;
        }
        break;

    case s_learn:
        g_imm_code_learn[cur_byte++] = byte;
        if (cur_byte >= IMM_CODE_LEN) {
            g_state = s_idle;

            /* Store only changed bytes to reduce EEPROM wear. */
            for (i = 0; i < IMM_CODE_LEN; ++i) {
                if (g_imm_code[i] != g_imm_code_learn[i]) {
                    g_imm_code[i] = g_imm_code_learn[i];
                    eeprom_write_byte((uint8_t *)i, g_imm_code[i]);
                }
            }
        }
        break;

    default:
        break;
    }
}

/*
 * Timer1 compare A: sample at approximately 75% of the current bit period.
 * A complete frame is one start bit followed by eight data bits.
 */
ISR(TIMER1_COMPA_vect)
{
    if (is_writing_byte())
        return;

    g_readed_bits = (g_readed_bits << 1) |
                    (((uint8_t)(~PINB) >> RX_INV_PIN) & 1);

    /* Bit 9 is the inverted start bit; when it is zero, a byte is complete. */
    if ((g_readed_bits & 0x0100) == 0) {
        handle_recv_byte((uint8_t)g_readed_bits & 0xFF);
        g_readed_bits = 0xFFFF;
    }
}

/* Timer1 compare B: end-of-bit timing and transmit state machine. */
ISR(TIMER1_COMPB_vect)
{
    static uint8_t cur_byte = 0;
    static uint8_t cur_bit = 9;

    TCNT1 = 0;

    switch (g_state) {
    case s_waitimm:
        if (g_imm_answer_timeout > 0) {
            if (is_reading_byte()) {
                g_imm_learn_timeout = 176;
                g_state = s_learn;
            } else {
                --g_imm_answer_timeout;
            }
        } else {
            g_state = s_simulate;
        }
        break;

    case s_learn:
        if (g_imm_learn_timeout > 0)
            --g_imm_learn_timeout;
        else
            g_state = s_idle;
        break;

    case s_simulate:
        if (cur_bit > 9 || cur_bit == 0) {
            /* Idle/space: TX low, Q2 off, bus released. */
            PORTB &= ~(1 << TX_INV_PIN);
        } else if (cur_bit == 9) {
            /* Start bit: TX high, Q2 on, bus pulled low. */
            PORTB |= (1 << TX_INV_PIN);
        } else {
            /* Data bits, LSB first, with the external transistor inversion. */
            if (g_imm_code[cur_byte] & (1 << (cur_bit - 1)))
                PORTB &= ~(1 << TX_INV_PIN); /* Logic 1 */
            else
                PORTB |= (1 << TX_INV_PIN);  /* Logic 0 */
        }

        if (cur_bit > 0) {
            --cur_bit;
        } else {
            cur_bit = 18; /* 18..10 space, 9 start, 8..1 data, 0 idle */
            ++cur_byte;

            if (cur_byte >= IMM_CODE_LEN) {
                /* No space before the first byte; waitimm supplied the delay. */
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
