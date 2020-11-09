/*
* dsp.c
*
* Created: 11/09/2020
* Author: Ankush Patel, Rukin Swedlund
*
* Functions regarding signal processing and calculations
*/

#define DSP_C /* for inclusion of "dsp.h" */

#include "common.h"
#include "dsp.h"
#include "adc.h"
#include "timer0.h"

#include <string.h>
#include <math.h>
#include <avr/interrupt.h>

#define PI 3.141592
#define CUBE(x) (x * x * x)
#define SQUARE(x) (x * x)

#define V_RMS_CORRECTION 0 //0.1
#define POWER_CORRECTION 0 //0.01

volatile unsigned currently_sampling = 0;

/* Inital channel is voltage (we sample voltage first, then current)
 * (Note the ZC interrupt will start with the opposit of the current channel)
 */
int current_adc_channel = ADC_CH_CURRENT;

/* Raw Voltage and Current Readings (Along with time value of each reading) */
volatile unsigned adc_voltages[RAW_ARRAY_SIZE];
volatile unsigned adc_voltages_head; /* index to next free space in array */

volatile unsigned adc_currents[RAW_ARRAY_SIZE];
volatile unsigned adc_currents_head;  /* index to next free space in array */

/* Reverse Gained array of values */
float raw_voltages[RAW_ARRAY_SIZE];
float raw_currents[RAW_ARRAY_SIZE];

float interpolated_voltages[INTERPOLATED_ARRAY_SIZE];
float interpolated_currents[INTERPOLATED_ARRAY_SIZE];

/* Array of (interpolated) V * (interpoloated) I */
float instantanous_power[INTERPOLATED_ARRAY_SIZE];

#ifdef HARDWARE_BUILD
static const float period = 0.002;
#else
static const float period = 0.02;
#endif /* HARDWARE_BUILD */

float power;
float rms_voltage;
float pk_current;
float energy = 0;

/* Zero Crossing Interrupt */
/* Currently sampling one cycle of the waveform at a time */
static volatile int16_t elapsed_cycle_time = 0;

/* Using Simpson's Rule */ 
static float numerical_intergreat(float *input)
{
	float numericalResult = input[0];

	for(uint8_t i = 1; i < 37 ; i= i+2){
		numericalResult = numericalResult + input[i] * 4;
		numericalResult = numericalResult + input[i + 1] * 2;
	}

	numericalResult = numericalResult + input[37] * 4;
	numericalResult = numericalResult + input[38];
	numericalResult = numericalResult * (50e-6 / 3);

	return numericalResult;
}

/* Using Trapezoidal Rule */
static float numerical_intergreat_trp(float *input)
{
	uint8_t i = 0;
	float deltaX = 0.00005;
	float numericalResult = input[0];

	for(i = 1; i < INTERPOLATED_ARRAY_SIZE - 1; ++i){
		numericalResult += 2 * input[i];
	}

	numericalResult += input[i];
	numericalResult = (deltaX / 2) * numericalResult;

	return numericalResult;
}

/* Cubic interpolate between a two points */
static float cubic_point(float x, float y0, float y1, float y2, float y3)
{
	float a = (-1.f / 2 * y0) + (3.f / 2 * y1) - (3.f / 2 * y2) + (1.f / 2 * y3);
	float b = y0 - (5.f / 2 * y1) + (2.f * y2) - (1.f / 2 * y3);
	float c = (-1.f / 2 * y0) + (1.f / 2 * y2);
	float d = y1;
		
	return (a * CUBE(x)) + (b * SQUARE(x)) + (c * x) + (d);
}

/* Cubic interpolate the raw arrays */
void cubic_interpolate()
{
	unsigned i, j;

	/* Voltage */
	/* First point is the same */
	i = j = 0;
	/* First point is the same */
	interpolated_voltages[j++] = raw_voltages[i];
	/* Point between first and second point is calculated a little differently (most inaccurate) */
	interpolated_voltages[j++] = cubic_point(0.5, 2 * raw_voltages[i] - raw_voltages[i + 1], raw_voltages[i], raw_voltages[i + 1], raw_voltages[i + 2]);
	for (++i; i < 20 - 2; ++i) {
		/* Original Point (y0) */
		interpolated_voltages[j++] = raw_voltages[i];
		/* Create new (Missing) Mid-Point */
		interpolated_voltages[j++] = cubic_point(0.5, raw_voltages[i - 1], raw_voltages[i], raw_voltages[i + 1], raw_voltages[i + 2]);
	}
	/* Point between second-to-last and last point is calculated a little differently (most inaccurate) */
	interpolated_voltages[j++] = raw_voltages[i];
	interpolated_voltages[j++] = cubic_point(0.5, raw_voltages[i - 1], raw_voltages[i], raw_voltages[i + 1], 2 * raw_voltages[i + 1] - raw_voltages[i]);
	interpolated_voltages[j++] = raw_voltages[++i];

	/* Current */
	i = j = 0;
	/* First point is the same */
	interpolated_currents[j++] = raw_currents[i];
	/* Point between first and second point is calculated a little differently (most inaccurate) */
	interpolated_currents[j++] = cubic_point(0.5, 2 * raw_currents[i] - raw_currents[i + 1], raw_currents[i], raw_currents[i + 1], raw_currents[i + 2]);
	for (++i; i < 20 - 2; ++i) {
		/* Original Point (y0) */
		interpolated_currents[j++] = raw_currents[i];
		/* Create new (Missing) Mid-Point */
		interpolated_currents[j++] = cubic_point(0.5, raw_currents[i - 1], raw_currents[i], raw_currents[i + 1], raw_currents[i + 2]);
	}
	/* Point between second-to-last and last point is calculated a little differently (most inaccurate) */
	interpolated_currents[j++] = raw_currents[i];
	interpolated_currents[j++] = cubic_point(0.5, raw_currents[i - 1], raw_currents[i], raw_currents[i + 1], 2 * raw_currents[i + 1] - raw_currents[i]);
	interpolated_currents[j++] = raw_currents[++i];
}

void calculate_power()
{
	unsigned i;

	for (i = 0; i < INTERPOLATED_ARRAY_SIZE; ++i)
		instantanous_power[i] = interpolated_voltages[i] * interpolated_currents[i];

	power = numerical_intergreat(instantanous_power) / period + POWER_CORRECTION;
}

/* NOTE: This funciton must be called after calculating power !! */
void calculate_energy()
{
	energy += (power * period)/(float)60;
}

/* NOTE the RMS Voltage calculation should be done last as it overrides the
 * interpolated_voltages array with the square of the interpolated voltages
 */
void calculate_rms_voltage()
{
	unsigned i;
	float correction = V_RMS_CORRECTION;

	/* WARNING: interpolated_voltages IS NOW SQUARED !!! */
	for (i = 0; i < INTERPOLATED_ARRAY_SIZE; ++i) {
		interpolated_voltages[i] = SQUARE(interpolated_voltages[i]);
	}
	
	rms_voltage = sqrt(numerical_intergreat(interpolated_voltages) / period);
	
#ifdef HARDWARE_BUILD
	/* Note error is non-linear (larger error towards middle) */
	
	/*if((12.7 <= rms_voltage) && (rms_voltage <= 15.7)){
		float percentage = rms_voltage/15.7;	
		correction = percentage*0.3 + 0.2;
	}*/
	/*
	if(rms_voltage < 12.7) correction = 0.2;
	if(rms_voltage < 13.6) correction = 0.4;
	if(rms_voltage < 14.5) correction = 0.6;
	if(rms_voltage < 15.4) correction = 0.4;
	if(rms_voltage > 15.4) correction = 0.2;
	*/
	
	//correction = (-0.164*((rms_voltage - 12.7)*(rms_voltage - 15.4))) + 0.2;

	//rms_voltage -= correction;

	#else
	rms_voltage += correction; //0.1V
#endif /* HARDWARE_BUILD */ 
}

void calculate_pk_current()
{
#ifdef HARDWARE_BUILD
	const float correction = 0; //0.2;
#else
	const float correction = 0;
#endif /* HARDWARE_BUILD */ 
/*	
	unsigned i;
	for (pk_current = i = 0; i < INTERPOLATED_ARRAY_SIZE; ++i)
		if (interpolated_currents[i] > pk_current)
			pk_current = interpolated_currents[i] + correction;
*/
#define TEST_PK_CURRENT

#ifdef TEST_PK_CURRENT
	/* Test this */
	{
	unsigned i;
	float rms_current;

	/* calculate rms current */
	/* WARNING: interpolated_currents IS NOW SQUARED !!! */
	for (i = 0; i < INTERPOLATED_ARRAY_SIZE; ++i) {
		interpolated_currents[i] = SQUARE(interpolated_currents[i]);
	}
	rms_current = sqrt(numerical_intergreat(interpolated_currents) / period);
	/* calculate peak current by doing rms * sqrt(2) */
	pk_current = rms_current * sqrt(2);
	}
#endif /* TEST_PK_CURRENT */
}


/* Convert ADC Value (0 - 1023) to Real Analogue Sensor Voltage Value */
void adc2real_voltage()
{
	int i;
	float vOffset = 2.1;
		
	// Voltage divider inverse gain
	const uint16_t Rb1 = 3300;
	const uint16_t Ra1 = 56000;
	float dividerGain = 1/ (float) (Rb1 / (float) (Ra1 + Rb1));
	
	
	// Voltage amplifier gain
	const uint16_t R2 = 4700;
	const uint16_t R1 = 4700;
	float amplifierGain =  1 / (float) (R2 / ((float) R1));

	for (i = 0; i < RAW_ARRAY_SIZE; ++i) {
		/* The voltage values in raw_currents are actually the ADC values
		 * Overwrite them with the actual raw voltage values
		 */
		/* raw_voltages[i] = (float) 0.0877427 * raw_voltages[i] - (float) 37.7365; */
		raw_voltages[i] = ((5 * adc_voltages[i] / 1024.f)); // - vOffset) * dividerGain * amplifierGain;
	}
	
#ifdef HARDWARE_BUILD

	float pk_voltage;
	uint8_t pk_voltage_index = 0;

	vOffset = 2.008;
	
	/* Finds the peak in the raw voltage array */
	for (pk_voltage = i = 0; i < RAW_ARRAY_SIZE; ++i){
		if (raw_voltages[i] > pk_voltage){
			pk_voltage = raw_voltages[i];
			pk_voltage_index = i;
		}
	}
	
	/* Accounts for UNKOWN distortions*/
	for(i = 0; i <= pk_voltage_index; ++i){
		raw_voltages[i] += 0.1;
	}
		
#endif /* HARDWARE_BUILD */	

	/* Apply offset cancel and reverse gain */
	for (i = 0; i < RAW_ARRAY_SIZE; ++i) {
		/* The voltage values in raw_currents are actually the ADC values
		 * Overwrite them with the actual raw voltage values
		 */
		/* raw_voltages[i] = (float) 0.0877427 * raw_voltages[i] - (float) 37.7365; */
		raw_voltages[i] = (raw_voltages[i] - vOffset) * dividerGain * amplifierGain;
	}
}

/* Convert ADC Value (0 - 1023) to Real Analogue Sensor Voltage Value */
void adc2real_current()
{
	int i;
	float vOffset = 2.1;
	
	// Voltage divider inverse gain
	
#ifdef HARDWARE_BUILD
	const float Rs1 = 0.5;
	
	vOffset = 2.008;
#else
	const float Rs1 = 0.56;
#endif

	float dividerGain = 1 / (float) (Rs1);
	
	// Voltage amplifier gain
	const uint16_t R2 = 56000;
	const uint16_t R1 = 22000;
	float amplifierGain =  1 / (float) (R2 / ((float) R1));
	
	for (i = 0; i < RAW_ARRAY_SIZE; ++i) {
		/* The voltage values in raw_currents are actually the ADC values
		 * Overwrite them with the actual raw voltage values
		 */
		/* raw_currents[i] = 0.0034253 * raw_currents[i] - 1.4731673; */
		raw_currents[i] = ((5 * adc_currents[i] / 1024.f) - vOffset) * dividerGain * amplifierGain;
	}
}

ISR(INT0_vect)
{
	extern volatile unsigned enable_zc;
	//Use this LED to check if interrupt is called.
	TGL_PORT(PORTB, PORTB5);
	
	/* Zero crossing indicates the start or end of a cycle of sampling */
	if (!currently_sampling && enable_zc) {
		currently_sampling = 1;
		adc_voltages_head = adc_currents_head = 0;
		/* Change the channel we will sample next */ 
		if (current_adc_channel == ADC_CH_VOLTAGE) {
			adc_set_channel(ADC_CH_CURRENT);
		} else {
			adc_set_channel(ADC_CH_VOLTAGE);
		}
		timer0_reset();
	} else if (currently_sampling && enable_zc) {
		currently_sampling = 0;
		enable_zc = 0;
		timer0_stop();
		/* Force sample the ADC one more time to get 20 samples */
		SET_PORT(ADCSRA, ADSC);
	}
}

/* Initializes voltage zero crossing interrupt */
void voltage_zc_interrupt_init()
{
	SET_PORT(EIMSK, INT0);

	/* Rising edge trigger */
	SET_PORT(EICRA, ISC01), SET_PORT(EICRA, ISC00);
	
	/* Changing Edge trigger 
	EICRA |= (1 << ISC00); 
	EICRA &= ~(1 << ISC01); */
}
