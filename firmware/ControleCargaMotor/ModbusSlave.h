/*
 *		ModBusSlave.h
 *
 *		Versão: 03-2022
 *      Author: Kerschbaumer (com pequenas modificações feitas por Clecio Jung)
 *
 *  Funções implementadas:
 *
 *  	Read Holding Registers (FC=03)
 *  	Preset Single Register (FC=06)
 *  	Preset Multiple Registers (FC=16)
 */

// Configuração ModBus
#define endereco_modbus 1 // endereço inicial da modbus, pode ser mudado depois
#define num_reg_words_modbus 3 // número de registradores (words) usados na modbus (variável data_word) funções 3 e 16
#define tam_buff_recep 255
#define tam_buff_trans 255
#define TxDelay 100 // TX Delay ms
// final Configuração ModBus

// configuração da serial
#define USART_BAUDRATE 19200 // taxa de transmissão desejada
#define BAUD_PRESCALE ((F_CPU / (USART_BAUDRATE * (long)16))-1) //calcula o valor do prescaler da usart
// fim da configuração da serial

// configuração do pino do driver RS485
#define ModBusTxEnablePort PORTD
#define ModBusTxEnablePin PD2
#define ModBusTxEnableDDR DDRD

#define ModBusRxEnable() ModBusTxEnablePort &= ~(1<<ModBusTxEnablePin) // Habilita a recepção do driver RS485
#define ModBusTxEnable() ModBusTxEnablePort |= (1<<ModBusTxEnablePin) // Habilita a transmissão do driver RS485

unsigned int ModBusTimerCont = 1;
unsigned int ModBusTimerInterval = 0;

struct mb
{
	enum e_status {aguardando, recebendo, processando, ignorando, iniciandoTransmisao, transmitindo} status;
	uint8_t end_modbus; // armazena o endereço na modbus
	uint16_t rxsize; // tamanho do pacote na recepção
	uint16_t txsize; // tamanho do pacote na transmissão
	uint8_t rxbuf[tam_buff_recep]; // buffer de recepção
	uint8_t txbuf[tam_buff_trans]; // buffer de transmissão
	uint8_t funcao;
	uint16_t data_reg[num_reg_words_modbus]; // dados de words a serem transmitidos e recebidos pela modbus
	uint16_t rxpt; // ponteiro para o buffer de recepçao, necessario em algumas arquiteturas
	uint16_t txpt; // ponteiro para o buffer de transmissão, necessario em algumas arquiteturas
} ModBus;

// Liga o temporizador usado na modBus com o intervalor ajustado para 1ms
// Ajustar para o clock utilizado
void inicia_timer_1ms()
{
	OCR2 = 250;			// Ajusta o valor de comparação do timer 2
	TCNT2 = 0;			// Zera a contagem do timer 2
	TCCR2 = (1<<CS22);	// habilita o clock do timer 2 com prescaller /64
	TIMSK |= (1<<OCIE2);	// habilita a interrupção do timer 2
}

// Liga o temporizador usado na modBus, deve ficar antes do #include "ModBusSlave.h"
void liga_timer_modbus(unsigned char t_ms)
{
	cli();
	ModBusTimerCont=0;
	ModBusTimerInterval=t_ms;
	inicia_timer_1ms();
	sei();
}

//	desliga o temporizador usado na modBus
#define desliga_timer_modbus() (TCCR2 = 0) // desliga o timer da modbus

// reseta o temporizador usado na modBus
#define reset_timer_modbus() (TCNT2 = 0) // reseta o timer da modbus

uint16_t CRC16Table256(uint16_t i) // gera tabela de crc
{
	uint16_t crc,c,j;
	crc = 0;
	c   = (uint16_t) i;
	for (j=0; j<8; j++)
	{
		if ( (crc ^ c) & 0x0001 ) crc = ( crc >> 1 ) ^ 0xA001;
		else crc =   crc >> 1;
		c = c >> 1;
    }
	return crc;
}

uint16_t update_crc_16(uint16_t crc, uint8_t c) // atualiza o valor do crc
{
	uint16_t tmp, short_c;
	short_c = 0x00ff & (uint16_t) c;
	tmp =  crc       ^ short_c;
	crc = (crc >> 8) ^ CRC16Table256((uint16_t) tmp & 0xff);
	return crc;
}

uint16_t CRC16(uint8_t *ptr,uint16_t npts) // calcula o crc de um vetor
{
	uint16_t crc;
	uint16_t i;
	crc=0xffff;// valor inicial para CRC16modbus
	for(i=0;i<npts;i++)
	{
		crc=update_crc_16(crc,(uint8_t) *(ptr+i));
    }
	return crc;
}

void ModBusReset()
{
	ModBusRxEnable();
	ModBus.rxsize=6;
	ModBus.end_modbus=endereco_modbus;
	ModBus.status=aguardando;
	ModBus.rxpt=0;
	ModBus.txpt=0;
}

void ModBusDefineFunction(uint8_t rchar)
{
	uint16_t tmp;
	if(rchar==3) //função 3 (identifica a função 3 do modbus)
	{
		ModBus.rxsize = 7; // prepara para receber 7 bytes conforme função 3
		ModBus.funcao=3;
		return;
	}
	if(rchar==6) //função 6 (identifica a função 6 do modbus)
	{
		ModBus.rxsize = 7; // prepara para receber 7 bytes conforme função 6
		ModBus.funcao=6;
		return;
	}
	if(rchar==16) //função 16 (identifica a função 16 do modbus)
	{
		tmp=(uint16_t)((ModBus.rxbuf[4]<<8)|ModBus.rxbuf[5]);
		ModBus.rxsize = 8+(tmp*2); // depende do número de registradores a ser alterados
		ModBus.funcao=16;
		return;
	}
	// função inválida, ignorando
	ModBus.status=ignorando;
	ModBus.rxsize = tam_buff_recep; // tamanho máximo	
}

void ModBusSendErrorMessage(uint8_t function, uint8_t code)
{
	uint16_t crc; // armazena o valor do crc do pacote
	ModBus.txbuf[0]=ModBus.end_modbus; // inicia o pacote de resposta com o endereço
	ModBus.txbuf[1]=function|0x80; // indica a função 1 na resposta com erro
	ModBus.txbuf[2]=code; // indica o número de registradores transmitidos em bytes
	crc=CRC16(ModBus.txbuf,3); // calcula o crc da resposta
	ModBus.txbuf[3]=(uint8_t)(crc&0x00ff); // monta 8 bits do crc para transmitir
	ModBus.txbuf[4]=(uint8_t)(crc>>8); // monta mais 8 bits do crc para transmitir
	ModBus.txsize=5; // armazena o tamanho do pacote para transmissão
	ModBus.status = iniciandoTransmisao; // atualiza o status para o main ativar o timer
	liga_timer_modbus(TxDelay);
}

void ModBusProcess()
{
	uint16_t crc; // armazena o valor do crc do pacote
	uint16_t temp; // variável para valores temporários
	uint16_t num_reg; // número de registradores que estão sendo lidos
	uint16_t cont_tx; // armazena o tamanho do pacote de transmissão
	uint16_t cont; // variável para contar os registradores transmitidos
	
	//verificar se é rxsize-1 ou rxsize-2 nas duas linhas a seguir
	crc=CRC16(ModBus.rxbuf,ModBus.rxsize-1); // calcula o crc do pacote
	if((uint8_t)(crc&0x00ff)==ModBus.rxbuf[ModBus.rxsize-1]&&(uint8_t)(crc>>8)==ModBus.rxbuf[ModBus.rxsize]) // testa se o crc é válido
	{
		if(ModBus.funcao==3) // se for a função 3
		{
			temp=(uint16_t)((ModBus.rxbuf[2]<<8)|ModBus.rxbuf[3]); //recebe o endereço dos registradores a serem lidos
			num_reg=(uint16_t)((ModBus.rxbuf[4]<<8)|ModBus.rxbuf[5]); // recebe a quantidade de registradores a serem lidos
			if((temp+num_reg-1)<num_reg_words_modbus) // verifica se é válido
			{
				ModBus.txbuf[0]=ModBus.end_modbus; // inicia o pacote de resposta com o endereço
				ModBus.txbuf[1]=3; // indica a função 3 na resposta
				ModBus.txbuf[2]=(uint8_t)(num_reg*2); // indica o número de registradores transmitidos em bytes
				cont_tx=3; // inicia o contador de tamanho do pacote de resposta
				for(cont=0; cont<num_reg; cont++) // conta os registradores enviados
				{
					ModBus.txbuf[cont_tx]=(uint8_t)((ModBus.data_reg[cont+temp])>>8); // envia os 8 bits mais altos do registrador
					cont_tx++; // incrementa o contador do tamanho da resposta
					ModBus.txbuf[cont_tx]=(uint8_t)((ModBus.data_reg[cont+temp])&0x00ff); // envia os 8 bits mais baixos do registrador
					cont_tx++; // incrementa o contador do tamanho da resposta
				}
				crc=CRC16(ModBus.txbuf,(uint16_t)((num_reg*2)+3)); // calcula o crc da resposta
				ModBus.txbuf[cont_tx]=(uint8_t)(crc&0x00ff); // monta 8 bits do crc para transmitir
				cont_tx++; // incrementa o contador do tamanho da resposta
				ModBus.txbuf[cont_tx]=(uint8_t)(crc>>8); // monta mais 8 bits do crc para transmitir
				cont_tx++; // incrementa o contador do tamanho da resposta
				ModBus.txsize=(uint8_t)(cont_tx); // armazena o tamanho do pacote para transmissão
				ModBus.status = iniciandoTransmisao; // atualiza o status para o main ativar o timer
				liga_timer_modbus(TxDelay);
			}
			else
			{
				ModBusSendErrorMessage(3, 2); // retorna erro de endereço ilegal
			}
		}

		if(ModBus.funcao==6) // se for a função 6
		{
			temp=(uint16_t)((ModBus.rxbuf[2]<<8)|ModBus.rxbuf[3]); //recebe o endereço do registrador a ser gravado
			if(temp<num_reg_words_modbus) // verifica se é válido
			{
				ModBus.data_reg[temp]=((ModBus.rxbuf[4]<<8)|ModBus.rxbuf[5]); // grava o valor do registrador
				ModBus.txbuf[0]=ModBus.end_modbus; // inicia o pacote de resposta com o endereço
				ModBus.txbuf[1]=6; // indica a função 16
				ModBus.txbuf[2]=ModBus.rxbuf[2]; // retorna 8 bits do dado gravado
				ModBus.txbuf[3]=ModBus.rxbuf[3]; // retorna 8 bits do dado gravado
				ModBus.txbuf[4]=ModBus.rxbuf[4];
				ModBus.txbuf[5]=ModBus.rxbuf[5];
				crc=CRC16(ModBus.txbuf,6); // calcula o crc da resposta
				ModBus.txbuf[6]=(uint8_t)(crc&0x00ff); // monta 8 bits do crc para transmitir
				ModBus.txbuf[7]=(uint8_t)(crc>>8); // monta mais 8 bits do crc para transmitir
				ModBus.txsize=8; // armazena o tamanho do pacote para transmissão
				ModBus.status = iniciandoTransmisao; // atualiza o status para o main ativar o timer
				liga_timer_modbus(TxDelay);
			}
			else
			{
				ModBusSendErrorMessage(6, 2); // retorna erro de endereço ilegal
			}
		}

		if(ModBus.funcao==16) // se for a função 16
		{
			temp=(uint16_t)((ModBus.rxbuf[2]<<8)|ModBus.rxbuf[3]); //recebe o endereço do registrador a ser gravado
			num_reg=(uint16_t)((ModBus.rxbuf[4]<<8)|ModBus.rxbuf[5]); // recebe a quantidade de registradores a serem gravados
			if((temp+num_reg-1)<num_reg_words_modbus) // verifica se é válido
			{
				ModBus.txbuf[0]=ModBus.end_modbus; // inicia o pacote de resposta com o endereço
				ModBus.txbuf[1]=16; // indica a função 16
				ModBus.txbuf[2]=ModBus.rxbuf[2]; // retorna 8 bits do dado gravado
				ModBus.txbuf[3]=ModBus.rxbuf[3]; // retorna 8 bits do dado gravado
				ModBus.txbuf[4]=ModBus.rxbuf[4]; // retorna 8 bits número de registradores gravados
				ModBus.txbuf[5]=ModBus.rxbuf[5]; // retorna 8 bits número de registradores gravados
				for(cont=0; cont<num_reg; cont++) // conta os registradores enviados
				{
					ModBus.data_reg[temp]=((ModBus.rxbuf[(cont*2)+7]<<8)|ModBus.rxbuf[(cont*2)+8]);
					temp++;
				}
				crc=CRC16(ModBus.txbuf,6); // calcula o crc da resposta
				ModBus.txbuf[6]=(uint8_t)(crc&0x00ff); // monta 8 bits do crc para transmitir
				ModBus.txbuf[7]=(uint8_t)(crc>>8); // monta mais 8 bits do crc para transmitir
				ModBus.txsize=8; // armazena o tamanho do pacote para transmissão
				ModBus.status = iniciandoTransmisao; // atualiza o status para o main ativar o timer
				liga_timer_modbus(TxDelay);
			}
			else
			{
				ModBusSendErrorMessage(16, 2); // retorna erro de endereço ilegal
			}
		}
	}
	else // CRC inválido
	{		
		ModBusReset();
	}
}

// Interrupção de recepção de caractere
ISR(USART_RXC_vect)
{
	cli();
	ModBus.rxbuf[ModBus.rxpt] = UDR; // recebe o byte
	reset_timer_modbus(); // reseta o timer da modbus
	if(ModBus.status==aguardando && ModBus.rxpt==0) // primeiro byte do pacote
	{
		liga_timer_modbus(1); // liga o timer para detectar pacotes truncados
	}

	if(ModBus.status==aguardando && ModBus.rxpt==6) // recebe o começo do pacote
	{
		if((ModBus.end_modbus != 0) && (ModBus.rxbuf[0]==ModBus.end_modbus)) //se o endereço confere inicia a recepção
		{
			ModBus.status=recebendo;
			ModBusDefineFunction(ModBus.rxbuf[1]); // seta a função
		}
		else // senão ignora o pacote
		{
			ModBus.status=ignorando;
			ModBus.rxsize = tam_buff_recep; // tamanho máximo
		}
	}
	if(ModBus.status==recebendo && ModBus.rxpt==ModBus.rxsize) // recebeu o restante do pacote
	{
		desliga_timer_modbus(); // desliga o timer da modbus
		ModBus.status = processando;
	}
	if(ModBus.rxpt<tam_buff_recep) ModBus.rxpt++; // incrementa o ponteiro de recepção se o tamanho não chegou no limite
	sei();
}

// Interrupção de fim de transmissão
ISR(USART_TXC_vect)
{
	cli();
	UCSRB &= ~(1 << TXCIE);	// desabilita a interrupção de final de transmissão
	ModBusReset();				// prepara para receber nova transmissão
	sei();
}

// Interrupção de caractere transmitido
ISR(USART_UDRE_vect)
{
	cli();
	if(ModBus.txpt==ModBus.txsize-1) // se transmitiu o penultimo caractere do pacote
	{
		UDR = ModBus.txbuf[ModBus.txpt]; // transmite o ultimo byte
		UCSRB &= ~(1 << UDRIE); // desabilita a interrupção e transmissão
		UCSRB |= (1 << TXCIE); // habilita a interrupção de final de transmissão
	}
	else // se ainda não é o ultimo byte do pacote
	{
		UDR = ModBus.txbuf[ModBus.txpt]; // transmite o byte
		ModBus.txpt++; // incrementa o ponteiro de transmissão
	}
	sei();
}

//Interrupção do temporizador
ISR(TIMER2_COMP_vect)
{
	cli();
	if(ModBusTimerCont<ModBusTimerInterval) ModBusTimerCont++;
	if(ModBusTimerCont==ModBusTimerInterval) // intervalo finalizado
	{
		ModBusTimerCont++;
		if(ModBus.status==iniciandoTransmisao)
		{
			ModBusTxEnable(); // Habilita a transmissão do driver 485 se necessário
			ModBus.status = transmitindo; // indica que está transmitindo
			desliga_timer_modbus(); // desliga o timer da modbus
			UDR = ModBus.txbuf[ModBus.txpt]; // transmite o primeiro byte, os seguintes são transmitidos na interrupção da serial
			UCSRB |= (1 << UDRIE); // habilita a interrupção da serial
			ModBus.txpt++; // incrementa o ponteiro de transmissção
		}
		
		if(ModBus.status==aguardando || ModBus.status==recebendo|| ModBus.status==ignorando) // se o timer disparou na recepção houve erro
		{
			ModBusReset(); // prepara para receber nova transmissão
			desliga_timer_modbus(); // desliga o timer da modbus
		}
	}
	sei();
}

//	Inicializa a comunicação serial
void usart_init()
{
	UCSRB = (1<<RXEN)|(1<<TXEN); // Turn on the transmission and reception circuitry
	UCSRC = (1<<USBS)|(1<<UCSZ1)|(3<<UCSZ0);
	UBRRL = BAUD_PRESCALE; // Load lower 8-bits of the baud rate value into the low byte of the UBRR register
	UBRRH = (BAUD_PRESCALE >> 8); // Load upper 8-bits of the baud rate value into the high byte of the UBRR register
	
	UCSRB |= (1 << RXCIE); // Enable the USART Recieve Complete interrupt (USART_RXC)

	ModBusTxEnableDDR |= (1<<ModBusTxEnablePin); // Habilita TX do driver RS485 como saída
	ModBusRxEnable(); // Habilita a recepção do driver RS485
}