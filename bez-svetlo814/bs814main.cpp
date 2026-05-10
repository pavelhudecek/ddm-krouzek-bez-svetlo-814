#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/eeprom.h>
#include <stdlib.h>

#define F_CPU	(10000000UL)
#define F_CPUkor (F_CPU + ((F_CPU * (int8_t)SIGROW.OSC20ERR5V) / 1024L))
#include "avr\delay.h"

#define COM_port	PORTA
#define COM_bp	1
#define COM_bm	(1<<COM_bp)
#define TLAC_port PORTA
#define TLAC_bm	(1<<0)

volatile uint8_t	ms10isrCnt=0;	// desetiny ms, max 25,0 ms pro funkci cekej
volatile uint16_t	ms10IsrMeas=0;	// desetiny ms pro mìøení

uint8_t		tlacitko=0;			// 1 stisknuto
uint16_t	tlacitkoTim=0;		// doba držení tlaèítka
uint16_t	modeSwTimLim=100;	// doba vedoucí k pøepnutí módu
bool		modeSwBylo=0;		// bylo zaregistrováno dosažení èasu pøepnutí

enum class ecMode : uint8_t {
	rot1r, rot1l, rot5r, rot5l,
	rotZtl, citacBin, rnd, postreh20, postrehDB,
	adc35, adc46, adcBin,
	enum_end
}; // mode = ecMode::rot1r;
template <typename E> E& operator++(E& e, int) { // operátor ++ pro enum
	uint8_t val = static_cast<uint8_t>(e);
	if (val < static_cast<uint8_t>(E::enum_end)) {
		e = static_cast<E>(val + 1);
	} else {
		e = static_cast<E>(0);
	}
	return e;
}

EEMEM uint32_t	eDummy = 1234567890UL;
EEMEM uint8_t	eMode = 0;

void eepromReset() { // =======================================================
	eeprom_update_dword(&eDummy, 1234567890UL);
	eeprom_update_byte(&eMode, 0);
}

uint8_t		jasKorPos = 1;		// 1-10 podle poètu LED
uint8_t		jasKorNeg = 1;		// 1-10 podle poètu LED
uint8_t		ledkyApos = 0;		// LEDky port A, pozitiv, akt. 1
uint8_t		ledkyAneg = 0;		// LEDkY port A, negativ, akt. 1
uint8_t		ledkyBpos = 0;		// LEDky port B, pozitiv, akt. 1
uint8_t		ledkyBneg = 0;		// LEDkY port B, negativ, akt. 1

void ledky(uint32_t val) { // nastavit stav LEDek =============================
	union {
		uint32_t val32;
		uint8_t val8;
	} tmp;
	tmp.val32 = val & ((1<<20)-1); // jen 20 bitù pro 20 LED
	
	uint8_t posA=0, posB=0, negA=0, negB=0; // porty A/B, neg/pos aktivní v 0/1
	uint8_t posCnt=0, negCnt=0;
	uint8_t b;
	
	for (uint8_t n=0; n<8; n++) { // B0-B3
		b = tmp.val8 & 1;
		if (n & 1) {// neg
			negB   |= b << (n >> 1);
			negCnt += b;
		} else {	// pos
			posB   |= b << (n >> 1);
			posCnt += b;
		}
		tmp.val32 >>= 1;
	}
	for (uint8_t n=0; n<12; n++) { // A7-A2
		b = tmp.val8 & 1;
		if (n & 1) {// neg
			negA |= b << 7 - (n >> 1);
			negCnt += b;
		} else {	// pos
			posA |= b << 7 - (n >> 1);
			posCnt += b;
		}
		tmp.val32 >>= 1;
	}
	__asm("cli");
	ledkyApos = posA;
	ledkyAneg = negA;
	ledkyBpos = posB;
	ledkyBneg = negB;
	jasKorPos = posCnt;
	jasKorNeg = negCnt;
	__asm("sei");
}

void bargraf(uint8_t val) { // nastavit stav LEDek jako bargraf ===============
	if (val>20) val=20;
	ledky((1UL << val) - 1UL);
}
void bargraf(float val) {
	bargraf(static_cast<uint8_t>(val*20.0));
}
void bargraf(uint16_t val) {
	bargraf(static_cast<uint8_t>(val));
}

void cekej(uint32_t dt) { // ==================================================
	uint32_t		t = 0UL;
	static uint8_t	tlacTmp, tlacLast=0, tlacCnt=0;
	
	if (dt>0) ms10isrCnt = 0;
	do {
		if (ms10isrCnt >= 10) { // každou ms, ale mùže dohnat až 25 promeškaných
			ms10isrCnt -= 10;
			t++;
			
			tlacTmp = ~TLAC_port.IN & TLAC_bm;
			if (tlacTmp==tlacLast) tlacCnt++;
			else tlacCnt = 0;
			tlacLast = tlacTmp;
			
			if (tlacCnt > 5) {
				tlacCnt = 5;
				tlacitko = tlacTmp;
			}
			if (tlacitko) {
				if (tlacitkoTim < 65535) tlacitkoTim++;
			} else {
				tlacitkoTim = 0;
			}
			if (tlacitkoTim>modeSwTimLim) {
				modeSwBylo = true;
			}

			/*ledky(
				tlacitkoTim 
				| (static_cast<uint32_t>(tlacitko)<<19)
				| (static_cast<uint32_t>(tlacTmp)<<18)
			);*/
		}
		
	} while (t<dt);
}


// lr: 0 vlevo / 1 vpravo, skup: poèet skupin, pocLed: LED ve skupinì
void bezSvetlo(uint8_t lr, uint8_t skup, uint8_t pocLed, uint8_t cas) { // ====
	uint32_t kruh = 0;
	uint8_t rozestup = 20/skup;
	
	modeSwTimLim = static_cast<uint16_t>(cas << 1);
	
	for (uint8_t s=0; s<skup; s++) {
		for (uint8_t d=0; d<pocLed; d++) {
			kruh |= 1 << s * rozestup + d;
		}
	}
	
	while (!modeSwBylo) for (
		uint8_t n = lr ? 19 : 0;
		n<20 && !modeSwBylo;
		lr ? n-- : n++
	) {
		uint32_t tmp;
		if (lr) {
			tmp = (kruh & (1UL << 19)) >> 19;
			kruh <<= 1;
		} else {
			tmp = (kruh & 1UL) << 19;
			kruh >>= 1;
		}
		kruh |= tmp;
		ledky(kruh);
		cekej(static_cast<uint32_t>(cas));
	} // konec for ve while
}

void rotZtlacitka() { // rotovat stream z tlacitka ============================
	uint32_t kruh = 0;
	
	modeSwTimLim = 3000;
	
	while (!modeSwBylo) {
		kruh <<= 1;
		kruh |= tlacitko;
		ledky(kruh);
		cekej(150);
	}
}

void citacBin() { // ==========================================================
	modeSwTimLim = 100;
	
	while (1) for (uint32_t cnt = 0; cnt<(1UL<<20); cnt++) {
		ledky(cnt);
		cekej(10);
		if (modeSwBylo) return;
	}
}

void postrehomer(uint8_t unit) { // ===========================================
	modeSwTimLim = 700;
	
	while (!modeSwBylo) {
		for (uint8_t n=0; n<20; n++) {
			ledky((unit<50 ? 1UL : 3UL) << n);
			cekej(150);
			if (modeSwBylo) return;
		}
		ledky((1UL<<20) - 1);
		cekej(rand() % 500 + 500);
		ledky(0);
		uint8_t cnt=20;
		if (!tlacitko) {
			for (cnt=1; cnt<21; cnt++) {
				cekej(unit);
				if (tlacitko) break;
				//bargraf(cnt);
				if (modeSwBylo) return;
			}
		}
		for (uint8_t n=0; n<20; n++) {
			bargraf(cnt);
			cekej(100);
			ledky(0);
			cekej(100);
			if (modeSwBylo) return;
		}
	}
}

void postrehomerDB() { // =====================================================
	uint16_t dbVals[] = { // 0 dB = 100 * 0,1 ms
		126, 158, 200, 251, 316, 398, 501, 630, 794, 1000, // 1-10 dB
		1259, 1585, 1995, 2512, 3162, 3981, 5012, 6309, 7943, 10000 // 11-20
	};
	modeSwTimLim = 700;
	
	while (!modeSwBylo) {
		for (uint8_t n=0; n<20; n++) {
			ledky(3UL << n);
			cekej(150);
			if (modeSwBylo) return;
		}
		ledky((1UL<<20) - 1);
		cekej(rand() % 500 + 500);
		ledky(0);
		__asm("cli");
		ms10IsrMeas = 0;
		__asm("sei");
		uint8_t cnt=20;
		if (!tlacitko) {
			for (cnt=1; cnt<21; cnt++) {
				while(ms10IsrMeas < dbVals[cnt-1]) cekej(0);
				if (tlacitko) break;
				//bargraf(cnt);
				if (modeSwBylo) return;
			}
		}
		for (uint8_t n=0; n<20; n++) {
			bargraf(cnt);
			cekej(100);
			ledky(0);
			cekej(100);
			if (modeSwBylo) return;
		}
	}
}

void adcMeas(uint8_t rozsah) { // mìøení napìtí na ADC ========================
	modeSwTimLim = 100;
	
	while (!modeSwBylo) {
		ledky(0);			// zhasnout aby mìøení nebylo rušeno
		cekej(1);			// poèkat na zhasnutí
		if (rozsah==0)	VREF_CTRLA = VREF_ADC0REFSEL_1V5_gc;
		else			VREF_CTRLA = VREF_ADC0REFSEL_2V5_gc; 
		ADC0_COMMAND = ADC_STCONV_bm;					   // Start ADC
		while (!(ADC0_INTFLAGS & ADC_RESRDY_bm)) cekej(0); // èekat na výsledek
		
		float volt = static_cast<float>(ADC0.RES) / 1023.0 / 64.0; // 0-1
		if (rozsah!=0) volt = 2.5/volt; // volt = 2,5/x ... x = 2,5/volt
		switch (rozsah) {
			case 0:	// bin mV
				volt = 1500.0/volt;
				ledky(static_cast<uint32_t>(volt));
				cekej(20);
				continue;		// continue vyskoèí až na zaèátek while
			case 35: // 3-5 V
				volt -= 3.0;
				volt /= 2.0;
			break;
			case 46: // 4-6 V
				volt -= 4.0;
				volt /= 2.0;
			break;
		}
		if (volt < 0.05) {
			// málo - bliknout 1 nebo 2 podle rozsahu
			ledky(rozsah==46 ? 3UL : 1UL); cekej(100);
			ledky(0); cekej(100);
		} else {
			bargraf(volt); // 0-1 -> 0-20 LED
			cekej(100);
		}
	}
}

int main(void) { // ###########################################################
	_PROTECTED_WRITE(CLKCTRL.MCLKCTRLB, CLKCTRL_PEN_bm); // f CPU /2, bude 10 MHz
	// ---------------------------------------------------------------------------------------------
	PORTA.DIR = 0;
	PORTA.OUT = 0;
	PORTA_PIN0CTRL = PORT_PULLUPEN_bm;			// TL
	PORTA_PIN1CTRL = PORT_ISC_INTDISABLE_gc;	// com
	PORTA_PIN2CTRL = PORT_ISC_INTDISABLE_gc;
	PORTA_PIN3CTRL = PORT_ISC_INTDISABLE_gc;
	PORTA_PIN4CTRL = PORT_ISC_INTDISABLE_gc;
	PORTA_PIN5CTRL = PORT_ISC_INTDISABLE_gc;
	PORTA_PIN6CTRL = PORT_ISC_INTDISABLE_gc;
	PORTA_PIN7CTRL = PORT_ISC_INTDISABLE_gc;
	PORTB.DIR = 0;
	PORTB.OUT = 0;
	PORTB_PIN0CTRL = PORT_ISC_INTDISABLE_gc;
	PORTB_PIN1CTRL = PORT_ISC_INTDISABLE_gc;
	PORTB_PIN2CTRL = PORT_ISC_INTDISABLE_gc;
	PORTB_PIN3CTRL = PORT_ISC_INTDISABLE_gc;
	// ------------------------------------------------------------------------
	TCA0.SINGLE.CTRLA = TCA_SINGLE_CLKSEL_DIV1_gc;
	TCA0.SINGLE.CTRLB = TCA_SINGLE_WGMODE_NORMAL_gc;
	TCA0.SINGLE.INTCTRL = TCA_SINGLE_OVF_bm;
	TCA0.SINGLE.PER = F_CPUkor / 10000UL;				// 10 kHz
	TCA0.SINGLE.INTFLAGS = TCA_SINGLE_OVF_bm;
	TCA0.SINGLE.CTRLA |= TCA_SINGLE_ENABLE_bm;
	// ------------------------------------------------------------------------
	VREF_CTRLA = VREF_ADC0REFSEL_2V5_gc;				// 2,5 V
	VREF_CTRLB = VREF_ADC0REFEN_bm;						// always on
	// ------------------------------------------------------------------------
	ADC0_CTRLA = ADC_RUNSTBY_bm + ADC_ENABLE_bm;		// Bìží i pøi sleepu + zapnout ADC
	ADC0_CTRLB = ADC_SAMPNUM_ACC64_gc;					// Zapnout akumulaci 64 vzorkù
	ADC0_CTRLC = ADC_REFSEL_VDDREF_gc + ADC_PRESC_DIV8_gc + ADC_SAMPCAP_bm; // VDD ref, clk/8=1,25 MHz ~ 100 kS/s, samp. C snížena
	ADC0_CTRLD = 0; //ADC_INITDLY_DLY16_gc + (2<<ADC_SAMPDLY_gp); // bez delay
	ADC0_CTRLE=0;										// Nepoužít okénkový komparátor
	ADC0_SAMPCTRL=0;									// Výchozí délka vzorku = 2+reg
	ADC0_MUXPOS=ADC_MUXPOS_INTREF_gc;					// pøepnout vstup na ref.
	//ADC0_COMMAND=ADC_STCONV_bm;							// Start Conversion
	ADC0_EVCTRL=0;										// nepoužívat eventy
	ADC0_INTCTRL = 0;									// nepoužívat pøerušení
	ADC0_INTFLAGS = ADC_RESRDY_bm;						// clear IF
	ADC0_DBGCTRL=0;										// not work in break
	ADC0_CALIB = ADC_DUTYCYC_DUTY25_gc;					// 25% Duty Cycle (high 25% and low 75%) must be used for ADCclk < 1.5 MHz
	// ---------------------------------------------------------------------------------------------
	__asm("sei"); // povolit pøerušení
	
	ledky((1UL<<20)-1);				 // test jestli všechny LEDky fungují
	do cekej(1000); while(tlacitko); // prodloužit test držením tlaèítka
	
	if ( // detekovat chybný obsah EEPROM
		eeprom_read_byte(&eMode) >= static_cast<uint8_t>(ecMode::enum_end)
		|| eeprom_read_dword(&eDummy) != 1234567890UL
	) {
		eepromReset();
	}
	ecMode mode = static_cast<ecMode>(eeprom_read_byte(&eMode));
	
    while (1) {
		switch(mode) {
			case ecMode::rot1r:
				bezSvetlo(1, 1, 2, 50);
			break;
			case ecMode::rot1l:
				bezSvetlo(0, 1, 2, 50);
			break;
			case ecMode::rot5r:
				bezSvetlo(1, 3, 3, 50);
			break;
			case ecMode::rot5l:
				bezSvetlo(0, 3, 3, 50);
			break;
			case ecMode::rotZtl:
				rotZtlacitka();
			break;
			case ecMode::citacBin:
				citacBin();
			break;
			case ecMode::postreh20:
				postrehomer(20);
			break;
			case ecMode::postrehDB:
				postrehomerDB();
			break;
			case ecMode::rnd:
				modeSwTimLim = 100;
				srand(12345);
				while (!modeSwBylo) {
					uint32_t kruh = rand();
					kruh |= (uint32_t)rand() << 15;
					ledky(kruh);
					cekej(30);
				}
			break;
			case ecMode::adc35:
				adcMeas(35); // 3-5 V
			break;
			case ecMode::adc46:
				adcMeas(46); // 4-6 V
			break;
			case ecMode::adcBin:
				adcMeas(0); // mV bin
			break;
		}
		ledky(0);
		while(tlacitko) {
			cekej(0);
			if (tlacitkoTim>10000) {
				for (uint8_t n=1; n<21; n++) {
					cekej(500);
					if (!tlacitko) break;
					bargraf(n);
					if (n==20) {
						eepromReset();
						mode = static_cast<ecMode>(static_cast<uint8_t>(ecMode::enum_end) - 1);
					}
				}
			}
		}
		modeSwBylo = 0;
		mode++;
		eeprom_update_byte(&eMode, static_cast<uint8_t>(mode));
    }
}

ISR (TCA0_OVF_vect) { // ======================================================
	static uint8_t mxCnt=0; // 0-19, perioda 2 ms
	
	if (ms10isrCnt<250) ms10isrCnt++;
	ms10IsrMeas++;
	if (++mxCnt>19)	 mxCnt=0;
	
	if (mxCnt<10) {
		if (mxCnt==0) {				// pos start
			COM_port.DIRCLR = COM_bm;
			PORTB.DIRCLR = 255;
			PORTA.DIRCLR = 255;
			PORTB.OUT = ledkyBpos;
			PORTA.OUT = ledkyApos & ~COM_bm;
			//COM_port.OUTCLR = COM_bm;
			PORTB.DIRSET = ledkyBpos;
			PORTA.DIRSET = ledkyApos | COM_bm;
			//COM_port.DIRSET = COM_bm;
		} else if (mxCnt==jasKorPos) {
			COM_port.DIRCLR = COM_bm;
		}
	} else {
		if (mxCnt==10) {			// neg start
			COM_port.DIRCLR = COM_bm;
			PORTB.DIRCLR = 255;
			PORTA.DIRCLR = 255;
			PORTB.OUT = ~ledkyBneg;
			PORTA.OUT = ~ledkyAneg | COM_bm;
			//COM_port.OUTSET = COM_bm;
			PORTB.DIRSET = ledkyBneg;
			PORTA.DIRSET = ledkyAneg | COM_bm;
			//COM_port.DIRSET = COM_bm;
		} else if (mxCnt==jasKorNeg+10) {
			COM_port.DIRCLR = COM_bm;
		}		
	}
	TCA0.SINGLE.INTFLAGS = TCA_SINGLE_OVF_bm;
}