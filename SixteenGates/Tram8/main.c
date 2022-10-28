/*
 * tram8 firmware for 16 gates - 8 binary (0V and 5V) and 8 dynamic (0-8V) 
 *
 * Created: 22.08.2017 16:49:13
 * Author : Kay Knofe
 * CC BY-NC-ND 4.0
 *
 * Version 1.3

 * 
 *
 */ 


#define BUTTON_LED_DEBUG

#include <avr/interrupt.h>
#include <avr/io.h>
#include <string.h>

#include <avr/eeprom.h>
//#include <avr/boot.h>

#define F_CPU 16000000UL
#define FOSC 16000000UL


#define BAUD 31250UL

#define MYUBRR (FOSC/(16*BAUD)-1)

#include <util/delay.h>
#include "MIDI.h"
#include "general_twi.h"
#include "MAX5825.h"
//#include "bootloader.h"

//PROTOTYPES
//void startupblink(void);
void midi_learn(void);
//void setup_process(void);
//void init_interrupt_pins(void);
void set_velocity(uint8_t ch, uint8_t velo);
void set_default(void);
void set_LED(uint8_t var);
//void start_timer0(uint8_t cmp_value);

void set_pin_inv(uint8_t pinnr);
void clear_pin_inv(uint8_t pinnr);

void fill_LUT(void);

#define ENABLE 1
#define DISABLE 0
#define TOGGLE 2

#define TRUE 1
#define FALSE 0

#define MODE_GATE 2
#define MODE_TRIGGER 1

#define EEPROM_CHANNEL_ADDR	(uint8_t *) 0x100 
#define EEPROM_MAP_ADDR		(uint8_t *) 0x101
#define EEPROM_MODE_ADDR	(uint8_t *) 0x112
//#define	EEPROM_PRESET_ADDR	(uint8_t *) 0x112

#define LED_pin PC0
#define BUTTON_PIN PC1

	#define BUTTON_UP		1
	#define BUTTON_DOWN		2
	#define BUTTON_PRESSED	3
	#define BUTTON_RELEASED	4




/* MIDI GLOBALS */

uint8_t midi_clock_tick_cntr=0;
uint8_t midi_clock_run=0;
uint8_t midi_quarter_cntr=0;
uint8_t midi_quarter_cntr_prev = 0;
uint8_t sync_counter_match = (1 << 2); //1=QRT 2=HLF 4=BAR 8=2BAR etc.
uint8_t wait_for_match = 0;
uint8_t boundary_led_flag = 0;
uint8_t group_hold = 0;

uint8_t midi_note_map[16] = {60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75};
uint8_t midi_note_map_default[16] = {60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75};

uint8_t midi_buff[3] = {0,0,0};
uint8_t midi_buff_point = 0;
uint8_t midi_buff_allowed = 0;
uint8_t midi_channel = 9;
uint8_t midi_learn_mode = 0;
uint8_t midi_learn_current = 0;

uint8_t module_mode = MODE_TRIGGER;

uint8_t velocity_out = 0;

uint16_t setting_wait_counter = 0;
uint8_t setting_wait_flag = 0;

uint8_t start_clock_delay_flag = 0;

//POINTER MANIPULATION 
void  (*set_pin_ptr)(uint8_t ) = & set_pin_inv;
void  (*clear_pin_ptr)(uint8_t ) = & clear_pin_inv;


int main(void)
{

/* GPIO INIT */
	//DDRA = 0xFF; //ROW&COLUMN FOR LED/BUTTON	
	DDRC = 0x0C | (1 << LED_pin) | (1 << BUTTON_PIN); //LDAC & CLEAR & LED
	DDRB = 0x01; // Trigger Out 0	
	DDRD = 0xFE; //Trigger outs 1-7
	//DDRE = 0xFC; //ENABLE outs 7-2
	//DDRG = 0x03; //ENABLE outs 1,0
    //DDRF = 0x0C; // 0,1 Button Ins 2,3 Clock outs 
   
	//PORTC |= (1 << BUTTON_PIN);
    PORTD |= 0xFE; //ALL GATES LOW (Inverter Out)
    PORTB |= 0x01; // 
	
	set_LED(ENABLE);
	_delay_ms(300);
	fill_LUT(); // change the velocity lookup table 
	set_LED(DISABLE);

	#define BUTTONFIXVARIABLE (uint8_t *) 0x07
	//THIS PART IS FOR HARDWARE VERSION 1.5 and up so Code is backwards compatible
	uint8_t buttonfix_flag = 0;

	do {} while (!eeprom_is_ready());
	//load Channel
	buttonfix_flag = eeprom_read_byte(BUTTONFIXVARIABLE);

	if (buttonfix_flag == 0xAA)
	{
		DDRC &= ~((1 << BUTTON_PIN));//INPUT
	}


	/* Button Poll Timer */
	TCCR2 = (1 << WGM20)|(0 << WGM21)|(0b111<<CS20);//CTC, PORT disconnected, /1024
	OCR2 = 157;//ca. 10ms@16MHz
	
	/* MIDI INIT */
	UCSRB = (1<<RXCIE)|(1<<RXEN);
	UCSRC = (1<<UCSZ0)|(1<<UCSZ1);
	UBRRH = (unsigned char)(MYUBRR>>8);
	UBRRL = (unsigned char) MYUBRR;
	
/* LOAD MIDI MAP FROM EEPROM*/
	do {} while (!eeprom_is_ready());
	//load Channel 
	midi_channel = eeprom_read_byte(EEPROM_CHANNEL_ADDR);
	do {} while (!eeprom_is_ready());
	//load map
	eeprom_read_block(&midi_note_map,EEPROM_MAP_ADDR,16);	
	do {} while (!eeprom_is_ready());
	//load Channel
	module_mode = eeprom_read_byte(EEPROM_MODE_ADDR);	
	
	
	
	do {} while (!eeprom_is_ready());
	//set map to default if never learned:
	if (midi_note_map[0]==0xFF)
		set_default();

	
/* CHECK FOR EXPANDERS */
	//uint8_t i2cread[5] = {NULL};
	//TWI_READ_BULK(0x20,0x00,2,&i2cread);
	PORTC = (1 << PC2)|(1 << PC3);	
	DDRC  |= (1 << PC2)|(1 << PC3);
	
	velocity_out = test_max5825();	//Velocity Out Expander present? (a.k.a. WK4) 
//	if(velocity_out){
		init_max5825();
		init_max5825();		
//		max5825_set_load_channel(0,0xFfff);
//	}

	//GET HARDWARE REVISION
	PORTB |= (1 << PB1); //PULLUP
	DDRB &= ~(1 << PB1); //INPUT
	_delay_ms(100);
	
	if ((PINB >> PB1) &1){
		//OLD HARDWARE
		//SET PINS INVERSE
		set_pin_ptr = & set_pin_inv;
		clear_pin_ptr = & clear_pin_inv;
		
		} else {
		//NEW HARDWARE - PIN TO GND
		//SET PINS NORMAL
		set_pin_ptr = & clear_pin_inv;
		clear_pin_ptr = & set_pin_inv;
	}

		(*clear_pin_ptr)(0);
		(*clear_pin_ptr)(1);
		(*clear_pin_ptr)(2);
		(*clear_pin_ptr)(3);
		(*clear_pin_ptr)(4);
		(*clear_pin_ptr)(5);
		(*clear_pin_ptr)(6);
		(*clear_pin_ptr)(7);


	midi_learn_mode = 0;
/* INTERRUPTS ENABLE */
	sei();
	
			//if((PINC_TEMP & 0x0F)==3) //GROUP&SYNC Presses
			//boot_main();	
			
	
/* MAIN LOOP */	
    while (1) {
	
	static uint8_t learn_button = BUTTON_UP;
	static uint8_t button_now = 0;
	static uint8_t button_bounce = 0;
	static uint8_t button_last = 0;
	

	//CHECK FOR GOTO SETTINGS MENU - LEARN BUTTON HAS TO BE DOWN FOR 3 SEC 
	if ((learn_button == BUTTON_RELEASED)){
		setting_wait_counter = 0;
		setting_wait_flag = 0;
		learn_button = BUTTON_UP;
	//	set_LED(DISABLE);
	}

	if (learn_button == BUTTON_PRESSED){
		learn_button = BUTTON_DOWN;
		//set_LED(ENABLE);		
		if(setting_wait_flag == 0){	//START THE WAIT
			setting_wait_counter = 0;
			setting_wait_flag = 1;
	
			//LEDs.groupLED = LED_BLINK2;

		}
	}
		
	if(learn_button == BUTTON_DOWN){
		if(setting_wait_flag == 1){
			if (setting_wait_counter >=200){
			//	set_LED(ENABLE);
				setting_wait_counter = 0;
				setting_wait_flag = 0;				
				learn_button = BUTTON_UP;
				midi_learn();			
			}
		}
	}
		
	

	/************************************************************************/
	/*                    keyscan                                           */
	/************************************************************************/
		if ((TIFR>>OCF2)&1)
		{
			//keyscan(&buttons);
			TCNT2 = 0; //reset timer
			TIFR |= (1 << OCF2); //reset flag		
			
			button_now = PINC & (1 << BUTTON_PIN);
			
			if (button_now != button_bounce){
				button_bounce = button_now;			
			}else{			
										
				if (button_now != button_last){			
					
					if(button_now == 0){
						learn_button = BUTTON_RELEASED;
						//set_LED(DISABLE);
					}else{
						learn_button = BUTTON_PRESSED;		
					//	set_LED(ENABLE);
					}
						
					button_last = button_now;		
				}			
			}

			if(setting_wait_flag == 1)
				setting_wait_counter++; 
		}
	 }
}

/************************************************************************/
/*						 MIDI LEARN			                            */
/************************************************************************/


void midi_learn(void)
{	
	static uint8_t learn_button = BUTTON_UP;
	static uint8_t button_now = 0;
	static uint8_t button_bounce = 0;
	static uint8_t button_last = 0;
	uint8_t blink_counter = 1;
	uint8_t firstrelease_flag = 1;
		
	midi_learn_mode = 1; //set global flag for MIDI RX ISR
	midi_learn_current = 0;
	//ALL OFF
	(*clear_pin_ptr)(0);
	(*clear_pin_ptr)(1);
	(*clear_pin_ptr)(2);
	(*clear_pin_ptr)(3);
	(*clear_pin_ptr)(4);
	(*clear_pin_ptr)(5);
	(*clear_pin_ptr)(6);
	(*clear_pin_ptr)(7);

	set_LED(ENABLE);
	
	
	while(midi_learn_current<16){
		
			//CHECK FOR LEAVE SETTINGS MENU - LEARN BUTTON HAS TO BE DOWN FOR 3 SEC
			if ((learn_button == BUTTON_RELEASED)){
				setting_wait_counter = 0;
				setting_wait_flag = 0;
				learn_button = BUTTON_UP;
	
				if (!firstrelease_flag){
					if (module_mode==MODE_TRIGGER)
						module_mode = MODE_GATE;
					else
						module_mode = MODE_TRIGGER;
				}
			
				firstrelease_flag = 0;
			}

			if (learn_button == BUTTON_PRESSED){
				learn_button = BUTTON_DOWN;
				//set_LED(ENABLE);
				if(setting_wait_flag == 0){	//START THE WAIT
					setting_wait_counter = 0;
					setting_wait_flag = 1;
				}
			}
			
			if(learn_button == BUTTON_DOWN){
				if(setting_wait_flag == 1){
					if (setting_wait_counter >=200){
						setting_wait_counter = 0;
						setting_wait_flag = 0;			
						midi_learn_current = 8; // leave 
					}
				}
			}//END BUTTON DOWN 

		
		
		
		
		
		/************************************************************************/
		/*                    keyscan                                           */
		/************************************************************************/
		if ((TIFR>>OCF2)&1)
		{
			//keyscan(&buttons);
			TCNT2 = 0; //reset timer
			TIFR |= (1 << OCF2); //reset flag
				
			button_now = PINC & (1 << BUTTON_PIN);
			
			blink_counter++;
			
			if ((blink_counter&0xF) == 0){
				if (module_mode==MODE_TRIGGER)
					set_LED(ENABLE);
				else
					set_LED(TOGGLE);
			}	
					
			if (button_now != button_bounce){
				button_bounce = button_now;
				}else{
					
				if (button_now != button_last){
						
					if(button_now == 0){
						learn_button = BUTTON_RELEASED;
						//set_LED(DISABLE);
						}else{
						learn_button = BUTTON_PRESSED;
						//	set_LED(ENABLE);
					}
						
					button_last = button_now;
				}
			}

			if(setting_wait_flag == 1)
			setting_wait_counter++;
		} //KEYSCAN END

	} //STAY HERE LOOP END
	
	midi_learn_mode=0;//DONE LEARNING!
	
	set_LED(DISABLE);


	//SAVE TO EEPROM 
	do {} while (!eeprom_is_ready());
	//save Channel
	//midi_channel = eeprom_read_byte(EEPROM_CHANNEL_ADDR);
	eeprom_write_byte(EEPROM_CHANNEL_ADDR,midi_channel);
	
	do {} while (!eeprom_is_ready());
	//save Channel
	//midi_channel = eeprom_read_byte(EEPROM_CHANNEL_ADDR);
	eeprom_write_byte(EEPROM_MODE_ADDR,module_mode);
			
	do {} while (!eeprom_is_ready());
	//load map
	//	eeprom_read_block(&midi_note_map,EEPROM_MAP_ADDR,8);
	eeprom_write_block(&midi_note_map,EEPROM_MAP_ADDR,16);

	//ALL OFF
	(*clear_pin_ptr)(0);
	(*clear_pin_ptr)(1);
	(*clear_pin_ptr)(2);
	(*clear_pin_ptr)(3);
	(*clear_pin_ptr)(4);
	(*clear_pin_ptr)(5);
	(*clear_pin_ptr)(6);
	(*clear_pin_ptr)(7);

	//end midi learn mode in MIDI RX ISR
	
	return;
	
}


/************************************************************************/

void set_default(void){
	//MIDI
	midi_channel = 9;
	module_mode = MODE_TRIGGER;
	memcpy(&midi_note_map,&midi_note_map_default,16);
	////clock divider
	////reset pulse edge
	//presets.clk_prescaler = DISABLE;
	//presets.reset_invert = DISABLE;
	//presets.velocity_mute = DISABLE;
	//presets.midi_conv_en = ENABLE;
	
	//SAVE TO EEPROM
	do {} while (!eeprom_is_ready());
	//save Channel
	//midi_channel = eeprom_read_byte(EEPROM_CHANNEL_ADDR);
	eeprom_write_byte(EEPROM_CHANNEL_ADDR,midi_channel);
	
	do {} while (!eeprom_is_ready());/*				SET DEFAULTS                                            */
/************************************************************************/


	//load map
	//	eeprom_read_block(&midi_note_map,EEPROM_MAP_ADDR,8);
	eeprom_write_block(&midi_note_map,EEPROM_MAP_ADDR,16);
	
	//SAVE TO EEPROM
	do {} while (!eeprom_is_ready());
	//save Channel
	//midi_channel = eeprom_read_byte(EEPROM_CHANNEL_ADDR);
	eeprom_write_byte(EEPROM_MODE_ADDR,module_mode);	
	
	//do {} while (!eeprom_is_ready());
	////load map
	////	eeprom_read_block(&midi_note_map,EEPROM_MAP_ADDR,8);
	//eeprom_write_block(&presets,EEPROM_PRESET_ADDR,sizeof(sys_presets));
	
	return;
}


/* MIDI RECEIVER */
ISR(USART_RXC_vect)
{
	uint8_t uart_data;
	uart_data = UDR;
	
	
	//set_LED(ENABLE);
	
	if ((uart_data>>MIDI_STATUS_bit)&1)
	{
		if((uart_data & 0xF8) != 0xF8)
			midi_buff_allowed = 0;		//reset running status only if not real time data
			
		if (((uart_data&0xE0) == MIDI_NOTE_OFF)){ //receives note ons too: &E0 !!
			midi_buff[0] = uart_data;
			midi_buff_point = 1;
			midi_buff_allowed = 2;
			//set_LED(ENABLE);
		}
		if ((uart_data&0xF0) == MIDI_CC){
			midi_buff[0] = uart_data;
			midi_buff_point = 1;
			midi_buff_allowed = 2;
			//set_LED(ENABLE);
		}
	
		
		} else if (midi_buff_allowed > 0) {
		//receive bytes of instruction if allowed
		midi_buff[midi_buff_point] = uart_data;
		midi_buff_point++;
		//midi_buff_allowed--;
		
		/************************************************************************/
		/*                    MIDI Buffer translate                             */
		/************************************************************************/
		
		if ((midi_buff_point == 3)) //3byte min for whole note message rx
		{
			midi_buff_point = 1;

			
			//LEARN MODE ONLY:
			if ((midi_learn_mode) && (midi_learn_current<16) && ((midi_buff[0]&0xF0)==MIDI_NOTE_ON)){//learn map from pressed button and midi RX		
				
				if (((midi_buff[0]&0x0F) != midi_channel)&&(midi_learn_current==0))
					midi_channel = (midi_buff[0]&0x0F); //IF CHANNEL 1 SELECTED ENABLE MIDI CHANNEL OVERRIDE
				
				if (((midi_buff[0]&0x0F) == midi_channel) && (midi_buff[2] != 0)){
					//ACTUALLY LEARN MAP FOR SELECTED CHANNEL HERE:
					midi_note_map[midi_learn_current] = midi_buff[1];
		
					
					//SET GATE ON CHANNEL TO SHOW STATUS
					(* set_pin_ptr)(midi_learn_current%8);					
					//if(midi_learn_current == 0)
						//PORTB &= 0xFE;//inverted cos of 74HC1G14 inverter 
					//else
						//PORTD &= 0xFF^(1 << midi_learn_current);//inverted cos of 74HC1G14 inverter 
		
					midi_learn_current++; //next channel	
					
					if(midi_learn_current==9){ //set the second set of midi notes for the dynamic trigs
						//(*clear_pin_ptr)(0);
						(*clear_pin_ptr)(1);
						(*clear_pin_ptr)(2);
						(*clear_pin_ptr)(3);
						(*clear_pin_ptr)(4);
						(*clear_pin_ptr)(5);
						(*clear_pin_ptr)(6);
						(*clear_pin_ptr)(7);
					}	
				}
			}
			
			
			//BUFFER TRANSLATE
			if (((midi_buff[0]&0x0F) == midi_channel) && !(midi_learn_mode))
			{
				uint8_t i = 0;
				
				
				if(((midi_buff[0]&0xF0) == MIDI_NOTE_ON)||((midi_buff[0]&0xF0) == MIDI_NOTE_OFF)){
					for (i=0;i<8;i++){
						if(midi_buff[1]==midi_note_map[i]){
							if (((midi_buff[0]&MIDI_NOTE_ON)==MIDI_NOTE_ON) && (midi_buff[2] != 0))//&&(midi_buff[2] != 0))
								(* set_pin_ptr)(i);
							else
								(*clear_pin_ptr)(i);
						}
					}//1-8
					for (i=8;i<16;i++){
						if(midi_buff[1]==midi_note_map[i]){
							if (((midi_buff[0]&MIDI_NOTE_ON)==MIDI_NOTE_ON) && (midi_buff[2] != 0))//&&(midi_buff[2] != 0))
								set_velocity((i&7),midi_buff[2]);
							else
								set_velocity((i&7),0); //back to 0V
						}
					}//9-16										
				}//NOTE ON OR NOTE OFF
			}
		}
	}
} //end ISR


void set_velocity(uint8_t ch, uint8_t velo){
	
	velo = velo & 0x7F; 
	
	max5825_set_load_channel((ch&0x0F),velocity_lookup[velo]);
	
	return;
}



void set_LED(uint8_t var){
	
	if (var==ENABLE)
		DDRC |= (1<<LED_pin);
	
	if(var==DISABLE)
		DDRC &= 0xFF^(1 << LED_pin);
	
	if(var==TOGGLE)
		DDRC ^= (1<<LED_pin);
	
	return;
}


//void start_timer0(uint8_t cmp_value){
	//TCNT0 = 0;
	//ASSR = 0;//(1 << AS0); //sync op - clock <= system clock
	//TCCR0 = (0 << WGM00)|(0 << WGM01)|(0b111<<CS00);//PORT disconnected, /1024
	//OCR0 = cmp_value; // 157;//ca. 10ms@16MHz
	//TIFR |= (1 << OCF0);	
	//return;
//}


void set_pin_inv(uint8_t pinnr){
	//inverted cos of 74HC1G14 inverter
	switch(pinnr){
		case 0: PORTB &= 0xFE;
		break;
		case 1: PORTD &= 0xFD;
		break;
		case 2: PORTD &= 0xFB;
		break;
		case 3: PORTD &= 0xF7;
		break;
		case 4: PORTD &= 0xEF;
		break;
		case 5: PORTD &= 0xDF;
		break;
		case 6: PORTD &= 0xBF;
		break;
		case 7: PORTD &= 0x7F;
		break;
		default: break;
	}
	
	return;
}

void clear_pin_inv(uint8_t pinnr){
	switch(pinnr){
		case 0: PORTB |= 0x01;
		break;
		case 1: PORTD |= 0x02;
		break;
		case 2: PORTD |= 0x04;
		break;
		case 3: PORTD |= 0x08;
		break;
		case 4: PORTD |= 0x10;
		break;
		case 5: PORTD |= 0x20;
		break;
		case 6: PORTD |= 0x40;
		break;
		case 7: PORTD |= 0x80;
		break;
		default: break;
	}
	
	return;
}

void fill_LUT(void){
	uint8_t i;
	
	velocity_lookup[127] = 0xFEF0; //not quite fulll scale cos the DAC doesn't like that sometimes
	
	for (i = 126; i==0; i--)
	{
		
		velocity_lookup[i] = (velocity_lookup[i+1] - 0x1C0) & 0xfff0; //mask on upper 12bits
	}
	
	velocity_lookup[0] = 0; //right, ey? 
	
	
	return;
}
