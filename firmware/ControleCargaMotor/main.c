//-------------------------------------------------------------------------------------------------------
#define F_CPU 16000000UL

#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>
#include "ModBusSlave.h"

//-------------------------------------------------------------------------------------------------------
// Constants

// LED test
#define SET_GREEN_LED()		(PORTC  |= (1<<PC4))
#define CLEAR_GREEN_LED()	(PORTC  &= ~(1<<PC4))
#define TOOGLE_GREEN_LED()	(PORTC  ^= (1<<PC4))
#define SET_RED_LED()		(PORTC  |= (1<<PC5))
#define CLEAR_RED_LED()		(PORTC  &= ~(1<<PC5))
#define TOOGLE_RED_LED()	(PORTC  ^= (1<<PC5))

// Dip Switch
#define DIP_SWITCH()		((uint16_t)((((~PIND) & 0xF0)) >> 4))

// ajustes das entradas analógicas
#define ANALOG_INPUT_GAIN	((uint32_t)1074)
#define ANALOG_INPUT_OFFSET ((uint32_t)3769)
#define CURRENT_GAIN		((int32_t)1311)
#define CURRENT_OFFSET		((int32_t)-45462)

// Máximos valores para o PWM e setpoint
#define PWM_MAX					800
#define PWM_LIMIT				180
#define SETPOINT_MAX			1000

// Parametros da entrada analógica
#define ANALOG_INPUT_TOO_LOW	150
#define ANALOG_INPUT_MIN		200
#define ANALOG_INPUT_MAX		1000

// Modo de controle de corrente (open or closed loop)
#define CLOSED_LOOP				1

// Controller adjust
#define GAIN_K1					200
#define GAIN_K2					190

//-------------------------------------------------------------------------------------------------------
// Global variables

/*	entrada analógica isolada 4-20mA
	range: 200-1000
	se o valor for < 200, a corrente é inferior a 4mA
	e provavelmente o cabo está rompido
*/
volatile uint16_t analog_input = 0;
/*	medição de corrente na carga 0-5A
	range: 0-1000
*/
volatile uint16_t current = 0;
/*	setpoint para controle de corrente
	range: 0-1000
*/
volatile uint16_t setpoint = 0;

//-------------------------------------------------------------------------------------------------------
// Run PI control

struct PI {
	int16_t previousError;
	int32_t controlSignal;
};

void piClear(struct PI *pi) {
	pi->previousError = 0;
	pi->controlSignal = 0;
}

struct PI piCurrent;

uint16_t piControl(struct PI *pi, uint16_t setPoint, uint16_t feedBack)
{
	int16_t error = setPoint - feedBack;
	pi->controlSignal += GAIN_K1 * error - GAIN_K2 * pi->previousError;
	pi->previousError = error;
	
	if (pi->controlSignal >= (int32_t)PWM_LIMIT*1000) {
		pi->controlSignal = (int32_t)PWM_LIMIT*1000;
	} else if (pi->controlSignal < 0) {
		pi->controlSignal = 0;
	}

	return (uint16_t)(pi->controlSignal/1000);
}

static inline void controle(void) {
	// operando com setpoint enviado pela entrada analógica
	if (ModBus.end_modbus == 0) {
		if (analog_input < ANALOG_INPUT_MIN) {
			setpoint  = 0;
		} else if (analog_input > ANALOG_INPUT_MAX) {
			setpoint = SETPOINT_MAX;
		} else {
			uint32_t sp32 = ((uint32_t)SETPOINT_MAX)*((uint32_t)(analog_input - ANALOG_INPUT_MIN));
			setpoint = (uint16_t) (sp32/((uint32_t)(ANALOG_INPUT_MAX - ANALOG_INPUT_MIN)));
		}
	}
	
	#if CLOSED_LOOP
		OCR1A = piControl(&piCurrent, setpoint, current);
	#else // open loop
		OCR1A = setpoint;
	#endif
}

static inline uint16_t dip_switch(void) {
	const uint16_t bit0 = (PIND&(1<<PD5)) ? 0:1;
	const uint16_t bit1 = (PIND&(1<<PD6)) ? 0:1;
	const uint16_t bit2 = (PIND&(1<<PD7)) ? 0:1;
	const uint16_t bit3 = (PINB&(1<<PB0)) ? 0:1;
	return ((bit3<<3)|(bit2<<2)|(bit1<<1)|(bit0<<0));
}

//-------------------------------------------------------------------------------------------------------
// ADC - Entradas Analógicas

// O ADC do Atmega8 não é muito bom. Segundo o datasheet, não deveriamos usar uma frequencia maior do que 200kHz,
// o que restringe bastante a frequência de operação. Além disso, o ADC consome 13 ciclos para fazer a medição,
// o que implica na frequência de amostragem máxima de 200/13 = 15,38kHz (medindo apenas um canal).
// E acima de tudo isso, não existe um meio para medir continuamente varios canais, temos de tratar isso no 
// codigo da interupção, o que torna a frequencia de amostragem variavel com o codigo gravado no processador.
ISR(ADC_vect)
{
	cli();
	// fs = 4.54kHz, T = 220us (medido experimentalmente com o osciloscópio)
	uint32_t adc = ADCW;
	
	// Le próximo canal
	ADMUX ^= (1<<MUX0);
	ADCSRA |= (1<<ADSC);
	
	if (ADMUX & (1<<MUX0)) {
		const int32_t current32 = (CURRENT_GAIN * (int32_t)adc + CURRENT_OFFSET)/1000;
		current = current32 > 0 ? (uint16_t)(current32) : 0;
		controle();
	} else {
		analog_input = (uint16_t)((ANALOG_INPUT_GAIN * (uint32_t)adc + ANALOG_INPUT_OFFSET)/1000);
	}
	sei();
}

//-------------------------------------------------------------------------------------------------------
int main(void)
{
	// Leds
	DDRC |= (1<<DDC4) | (1<<DDC5);
	// Dip Switch - Liga os resistores de pull-up
	PORTD |= (1<<PD7)|(1<<PD6)|(1<<PD5);
	PORTB |= (1<<PB0);
	
	//-----------------------------------
	// ADC - Entradas Analógicas
	// Frequencia de Amostragem = 4.8kHz (medição de dois canais em sequência)
	ADMUX = (1<<REFS0);
	// Prescaler = /128, conversion cycles = 13, f = 9.6kHz, T = 200us
	ADCSRA = (1<<ADEN)|(1<<ADIE)|(1<<ADPS2)|(1<<ADPS1)|(1<<ADPS0);
	// Inicia nova conversão
	ADCSRA |= (1<<ADSC);

	//-----------------------------------
	// Timer 1 - PWM
	PORTB |= (1<<PB1);
	TCNT1 = 0;
	OCR1A = 0;
	ICR1 = PWM_MAX; // Freq = 10kHz
	TCCR1A = (1 << COM1A1)|(1 << COM1A0)|(1 << WGM11); // Set OC1A on Compare Match and clear at BOTTOM
	TCCR1B = (1 << WGM13)|(1 << WGM12)|(1 << CS10); // Mode 14, Fast PWM, overflow on ICR1, set prescaler to 1
	DDRB |= (1<<DDB1);
	
	usart_init(); // inicia a comunicação serial utilizada na ModBus
	ModBusReset(); // prepara para receber a transmissão
	
	// Inicializa variaveis internas do controlador
	piClear(&piCurrent);
	
	// Enable interrupts
	sei();
	
	uint32_t counter = 0;
	
	// Main loop
	while (1)
	{
		counter++;
		if (counter >= 50000) {
			counter = 0;
			TOOGLE_GREEN_LED();
		}
		
		if (ModBus.status == aguardando || ModBus.end_modbus == 0) {
			uint8_t novo_endereco = dip_switch();
			if (ModBus.end_modbus != novo_endereco) {
				setpoint = 0; // reseta o setpoint para desligar o sistema
				piClear(&piCurrent);
				ModBus.data_reg[2] = 0;
				ModBus.end_modbus = novo_endereco;
			}
		}
		
		if (ModBus.end_modbus != 0)	{
			if (ModBus.status == processando) {
				ModBusProcess(); // inicia o processamento do pacote
			}
			ModBus.data_reg[0] = analog_input;
			ModBus.data_reg[1] = current;
			if (ModBus.data_reg[2] < SETPOINT_MAX) {
				setpoint = ModBus.data_reg[2];
			} else {
				setpoint = SETPOINT_MAX;
			}
		}
		
		if ((ModBus.end_modbus == 0) && (analog_input < ANALOG_INPUT_TOO_LOW)) {
			// liga led vermelho para indicar que a entrada analógica está recebendo menos de 4mA
			SET_RED_LED();
		} else {
			CLEAR_RED_LED();
		}
	}
}
//-------------------------------------------------------------------------------------------------------