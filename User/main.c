//
// Pocket CO2 detector
// by Larry Bank
// bitbank@pobox.com
// Copyright (c) 2023 BitBank Software, Inc.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
#include <stdint.h>
#include <string.h>
#include "debug.h"
#include "scd41.h"
#include "Arduino.h"
#include "oled.h"
#include "Roboto_Black_40.h"
#include "Roboto_Black_13.h"
#include "co2_emojis.h"

// end of 16k FLASH is at 0x08004000
#define FLASH_START 0x08003c00

#define DC_PIN 0xd3
#define CS_PIN 0xd2
#define RST_PIN 0xd4
#define BUTTON0_PIN 0xd2
#define BUTTON1_PIN 0xd3
#define LED_GREEN 0xc3
#define LED_RED 0xc4
#define MOTOR_PIN 0xc5

//#define DEBUG_MODE

typedef struct tagState
{
	int iMode;
	int iAlert;
	int iFreq;
	int iPeriod;
} STATE;

enum
{
	MODE_CONTINUOUS=0,
	MODE_LOW_POWER,
//	MODE_ON_DEMAND,
	MODE_STEALTH,
	MODE_CALIBRATE,
	MODE_TIMER,
	MODE_COUNT
};

enum
{
	ALERT_VIBRATION=0,
	ALERT_LED,
	ALERT_BOTH,
	ALERT_COUNT
};

enum
{
	MENU_START=0,
	MENU_MODE,
	MENU_FREQ,
	MENU_ALERT,
	MENU_TIME,
	MENU_COUNT
};

int GetButtons(void);
void ShowAlert(void);
void ShowTime(int iSecs);
void BlinkLED(uint8_t u8LED, int iDuration);

const char *szMode[] = {"Continuous", "Low Power ", /*"On Demand ", */ "Stealth   ", "Calibrate ", "Timer     "};
const char *szAlert[] = {"Vibration", "LEDs     ", "Vib+LEDs "};
STATE state;

static int iSample = 0; // number of CO2 samples captured
#ifdef FUTURE
#define MAX_SAMPLES 540
static uint8_t ucLast32[32]; // holds top 8 bits of last 32 samples
static uint8_t ucSamples[MAX_SAMPLES]; // 24h worth of samples (80 seconds each)
static int iHead = 0, iTail = 0; // circular list of samples
static int iMaxCO2 = 0, iMinCO2 = 5000;
static int iMaxTemp = 0, iMinTemp = 1000;
static uint8_t ucMaxHumid = 0, ucMinHumid = 100;
#endif // FUTURE

// Convert a number into a zero-terminated string
int i2str(char *pDest, int iVal)
{
	char *d = pDest;
	int i, iPlaceVal = 10000;
	int iDigits = 0;

	if (iVal < 0) {
		iDigits++;
		*d++ = '-';
		iVal = -iVal;
	}
	while (iPlaceVal) {
		if (iVal >= iPlaceVal) {
			i = iVal / iPlaceVal;
			*d++ = '0' + (char)i;
			iVal -= (i*iPlaceVal);
			iDigits++;
		} else if (iDigits != 0) {
			*d++ = '0'; // non-zeros were already displayed
		}
		iPlaceVal /= 10;
	}
	if (d == pDest) // must be zero
		*d++ = '0';
	*d++ = 0; // terminator
	return (int)(d - pDest - 1); // string length
} /* i2str() */

// Write the state variables to the 64-byte user FLASH memory area
void WriteFlash(void) {
int i;
uint32_t *s = (uint32_t *)&state;

    FLASH_Unlock_Fast();
    FLASH_ErasePage_Fast(FLASH_START);
    FLASH_BufReset();
    for(i=0; i<sizeof(state)/4; i++){
	    FLASH_BufLoad(FLASH_START+(4*i), s[i]);
    }
    FLASH_ProgramPage_Fast(FLASH_START);
    FLASH_Lock_Fast();
} /* WriteFlash() */

// Read the state variables from FLASH memory
void ReadFlash(void) {
int i;
uint32_t *d = (uint32_t *)&state;

	for (i=0; i<sizeof(state)/4; i++) {
		d[i] = *(uint32_t *)(FLASH_START + (4 * i));
	}
	if (state.iPeriod < 5 || state.iPeriod > 60) {
	// Data is not valid, set default values
        state.iMode = MODE_CONTINUOUS;
        state.iAlert = 0; // vibration only
        state.iFreq = 30; // stealth mode update time (30 seconds)
        state.iPeriod = 5; // wake up period in minutes
        WriteFlash(); // write the default values in FLASH
	}
} /* ReadFlash() */

#ifdef FUTURE
//
// Add a sample to the collected statistics
//
void AddSample(int i)
{
	if (_iCO2 > iMaxCO2) iMaxCO2 = _iCO2;
	if (_iCO2 < iMinCO2) iMinCO2 = _iCO2;
	ucLast32[i & 31] = (uint8_t)(_iCO2>>5); // keep top 8 bits
	if ((i & 31) == 0) {
		int iAvg = 0;
		// take average and store it
		for (int j=0; j<31; j++)
			iAvg += ucLast32[j];
		ucSamples[iHead++] = (uint8_t)(iAvg>>5);
		if (iHead >= MAX_SAMPLES) iHead -= MAX_SAMPLES; // wrap
	}
	if (_iTemperature > iMaxTemp)
		iMaxTemp = _iTemperature;
	else if (_iTemperature < iMinTemp)
		iMinTemp = _iTemperature;
	if ((_iHumidity/10) > ucMaxHumid)
		ucMaxHumid = (uint8_t)(_iHumidity/10);
	else if ((_iHumidity/10) < ucMinHumid)
		ucMinHumid = (uint8_t)(_iHumidity/10);
} /* AddSample() */
#endif // FUTURE

void EXTI7_0_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));

/*********************************************************************
 * @fn      EXTI0_IRQHandler
 *
 * @brief   This function handles EXTI0 Handler.
 *
 * @return  none
 */
void EXTI7_0_IRQHandler(void)
{
  if(EXTI_GetITStatus(EXTI_Line0)!=RESET)
  {
//    printf("EXTI0 Wake_up\r\n");
	  oledFill(0);
    EXTI_ClearITPendingBit(EXTI_Line0);     /* Clear Flag */
  }
}

void Option_Byte_CFG(void)
{
    FLASH_Unlock();
    FLASH_EraseOptionBytes();
    FLASH_UserOptionByteConfig(OB_IWDG_SW, OB_STOP_NoRST, OB_STDBY_NoRST, OB_RST_NoEN);
    FLASH_Lock();
}

#ifdef FUTURE
void ShowGraph(void)
{
	char szTemp[32];
	int i;

	I2CInit(400000);
	oledFill(0);

	i2str(szTemp, iHead*32);
    oledWriteString(0,0, szTemp, FONT_8x8, 0);
    oledWriteString(-1, 0, " Samples", FONT_8x8, 0);
    i = (iHead*32*5)/60; // number of minutes
    oledWriteString(0,8, "(", FONT_8x8, 0);
	i2str(szTemp, i);
    oledWriteString(-1,8, szTemp, FONT_8x8, 0);
    oledWriteString(-1,8, " minutes)", FONT_8x8, 0);
	oledWriteString(0,16,"CO2 level:",FONT_12x16, 0);
	oledWriteString(0,32,"Min:",FONT_8x8, 0);
	oledWriteString(0,40,"Max:",FONT_8x8, 0);
	oledWriteString(0,48,"Temp min/max: ",FONT_6x8, 0);
	oledWriteString(0,56,"Humi min/max: ", FONT_6x8, 0);

	i2str(szTemp, iMinCO2);
    oledWriteString(40, 32, szTemp, FONT_8x8, 0);
    i2str(szTemp, iMaxCO2);
    oledWriteString(40, 40, szTemp, FONT_8x8, 0);

	i2str(szTemp, iMinTemp/10); // whole part
    oledWriteString(84, 48, szTemp, FONT_6x8, 0);
    oledWriteString(-1, 48, "/", FONT_6x8, 0);
    i2str(szTemp, iMaxTemp/10);
    oledWriteString(-1, 48, szTemp, FONT_6x8, 0);
    oledWriteString(-1, 48, "C", FONT_6x8, 0);

    i2str(szTemp, ucMinHumid);
    oledWriteString(84, 56, szTemp, FONT_6x8, 0);
    oledWriteString(-1, 56, "/", FONT_6x8, 0);
    i2str(szTemp, ucMaxHumid);
    oledWriteString(-1, 56, szTemp, FONT_6x8, 0);
    oledWriteString(-1, 56, "%", FONT_6x8, 0);

    while (digitalRead(BUTTON0_PIN) == 0) {}; // wait for button to release
	while (digitalRead(BUTTON0_PIN) == 1) {}; // wait for button to press
	oledFill(0);
	while (digitalRead(BUTTON0_PIN) == 0) {}; // wait for button to release to exit
} /* ShowGraph() */
#endif // FUTURE
//
// Display the current conditions on the OLED
//
void ShowCurrent(void)
{
int i, x;
char szTemp[32];

	I2CSetSpeed(400000); // OLED can handle 400k
	i = i2str(szTemp, (int)_iCO2);
	oledWriteStringCustom(&Roboto_Black_40, 0, 32, szTemp, 1);
	x = oledGetCursorX();
	if (i < 4) {
	   oledWriteString(x+24, 0, "  ", FONT_12x16, 0); // make sure old data is erased if going from 4 to 3 digits
	   oledWriteString(x, 16, "   ", FONT_12x16, 0);
	}
	oledWriteString(x, 0, "CO2", FONT_8x8, 0);
	oledWriteString(x, 8, "ppm", FONT_8x8, 0);
    oledWriteStringCustom(&Roboto_Black_13, 0, 45, (char *)"Temp", 1);
    oledWriteStringCustom(&Roboto_Black_13, 0, 63, (char *)"Humidity", 1);
    i2str(szTemp, _iTemperature/10); // whole part
    oledWriteStringCustom(&Roboto_Black_13, 44, 45, szTemp, 1);
    oledWriteStringCustom(&Roboto_Black_13, -1, -1, ".", 1);
    i2str(szTemp, _iTemperature % 10); // fraction
    oledWriteStringCustom(&Roboto_Black_13, -1, -1, szTemp, 1);
    oledWriteStringCustom(&Roboto_Black_13, -1, -1, "C ", 1);
    i2str(szTemp, _iHumidity/10); // throw away fraction since it's not accurate
    oledWriteStringCustom(&Roboto_Black_13, 64, 63, szTemp, 1);
    oledWriteStringCustom(&Roboto_Black_13, -1, -1, "%", 1);
    // Display an emoji indicating the CO2 level
    // There are 5 which go from happy to angry, so divide the values into
    // 5 categories: 0-999, 1000-1499, 1500-1999, 2000-2499, 2500+
    x = (_iCO2 - 500)/500;
    if (x < 0) x = 0;
    else if (x > 4) x = 4;
    oledDrawSprite(96, 16, 31, 32, (uint8_t *)&co2_emojis[x * 4], 20, 1);
} /* ShowCurrent() */

void RunTimer(void)
{
  int i, j, iTicks = 5;
  oledFill(0);
//  oledContrast(20);
  oledWriteString(0,0, "Timer Mode", FONT_12x16, 0);
  for (i=state.iPeriod*60; i>=0; i--) { // count down seconds
	  ShowTime(i);
//	  Standby82ms(10); // sleep about 820ms
	  j = GetButtons();
	  if (j == 3) { // both buttons cancels timer mode
		  return;
	  }
	  if (j && iTicks == 0) { // a single button press turns on the display
		  iTicks = 5;
		  oledPower(1);
	  }
	  if (i == 10) { // turn on the display for the last 10 seconds
		  if (iTicks == 0) {
			  oledPower(1);
		  }
		  iTicks = 11;
	  }
	  if (iTicks) {
		  iTicks--;
		  if (iTicks == 0) {
			  oledPower(0); // turn off the display
		  }
	  }
	  BlinkLED((i & 1) ? LED_GREEN : LED_RED, 10);
	  Delay_Ms(990);
  }
  ShowAlert();
} /* RunTimer() */

void RunMenu(void)
{
int iSelItem = 0;
int y, bDone = 0;
char szTemp[16];
STATE oldstate = state;
pinMode(MOTOR_PIN, OUTPUT);
	   oledInit(0x3c, 400000);
	   oledFill(0);
	   oledContrast(150);
	   oledWriteString(4,0,"Pocket CO2", FONT_12x16, 0);
//	   oledWriteString(0,16,"================", FONT_8x8, 0);
	   while (!bDone) {
		   // draw the menu items and highlight the currently selected one
		   y = 24;
		   oledWriteString(0,y,"Start", FONT_8x8, (iSelItem == MENU_START));
		   y += 8;
		   oledWriteString(0,y, "Mode", FONT_8x8, (iSelItem == MENU_MODE));
		   oledWriteString(40,y, szMode[state.iMode], FONT_8x8, 0);
		   y += 8;
		   oledWriteString(0,y,"Update", FONT_8x8, (iSelItem == MENU_FREQ));
  		   i2str(szTemp, state.iFreq);
    	   oledWriteString(56,y, szTemp, FONT_8x8, 0);
    	   oledWriteString(-1,y, " secs", FONT_8x8, 0);
		   y += 8;
		   oledWriteString(0,y,"Alert", FONT_8x8, (iSelItem == MENU_ALERT));
		   oledWriteString(48,y,szAlert[state.iAlert], FONT_8x8, 0);
		   y += 8;
		   oledWriteString(0,y,"Timer", FONT_8x8, (iSelItem == MENU_TIME));
		   i2str(szTemp, state.iPeriod); // time in minutes
		   oledWriteString(48, y, szTemp, FONT_8x8, 0);
		   oledWriteString(-1,y, " Mins ", FONT_8x8, 0); // erase old value
		   // wait for button releases
		   while (GetButtons() != 0) {
			   Delay_Ms(20);
		   }
		   // wait for a button press
		   while (GetButtons() == 0) {
			   Delay_Ms(20);
		   }
		   y = GetButtons();
		   if (y & 1) { // button 0
		      iSelItem++;
		      if (iSelItem == MENU_COUNT) iSelItem = 0;
		      continue;
		   }
		   if (y & 2) { // button 1 - action on an item
			   switch (iSelItem) {
			   case MENU_START: // start
				   bDone = 1;
				   break;
			   case MENU_MODE: // mode
				   state.iMode++;
				   if (state.iMode >= MODE_COUNT) state.iMode = 0;
				   break;
			   case MENU_FREQ: // stealth update frequency
				   state.iFreq += 15;
				   if (state.iFreq > 60) state.iFreq = 15;
				   break;
			   case MENU_ALERT: // alert type
				   state.iAlert++;
				   if (state.iAlert >= ALERT_COUNT) state.iAlert = 0;
				   break;
			   case MENU_TIME: // time period
				   state.iPeriod += 5;
				   if (state.iPeriod > 60) state.iPeriod = 5;
				   break;
			   }
			   continue;
		   }
	   }; // while (!bDone)
	   // Check if any values changed; if so, write new values to FLASH
	   if (state.iMode != MODE_CALIBRATE && memcmp(&state, &oldstate, sizeof(state)) != 0)  {
		   WriteFlash(); // don't save settings for calibration mode
	   }
} /* RunMenu() */

void BlinkLED(uint8_t u8LED, int iDuration)
{
	pinMode(u8LED, OUTPUT);
    digitalWrite(u8LED, 1);
    Delay_Ms(iDuration);
    digitalWrite(u8LED, 0);
} /* BlinkLED() */

//
// Run the vibration motor
// for N milliseconds
// 50ms is a practical minimum
//
void Vibrate(int iDuration)
{
	pinMode(MOTOR_PIN, OUTPUT);
	digitalWrite(MOTOR_PIN, 1);
	Delay_Ms(iDuration);
	digitalWrite(MOTOR_PIN, 0);
} /* Vibrate() */

void ShowAlert(void)
{
int i;

	switch (state.iAlert)
	{
	case ALERT_VIBRATION:
		for (i=0; i<3; i++) {
		    Vibrate(150);
		//    Standby82ms(10);
		    Delay_Ms(820);
		  }
		break;
	case ALERT_LED:
		for (i=0; i<4; i++) {
		    BlinkLED(LED_GREEN, 300);
		    BlinkLED(LED_RED, 300);
		  }
		break;
	case ALERT_BOTH:
		for (i=0; i<3; i++) {
		    Vibrate(150);
		    BlinkLED(LED_GREEN, 400);
		    BlinkLED(LED_RED, 400);
		  }
		break;
	}
} /* ShowAlert() */

void ShowTime(int iSecs)
{
	char szTemp[8];
    szTemp[0] = '0';
	szTemp[1] = (iSecs/60)+'0';
	szTemp[2] = ':';
	szTemp[3] = ((iSecs % 60) / 10) + '0';
	szTemp[4] = (iSecs % 10) + '0';
	szTemp[5] = 0;
	oledWriteStringCustom(&Roboto_Black_40, 10, 56, szTemp, 1);
//	oledWriteString(34,24,szTemp, FONT_12x16, 0);
} /* ShowTime() */

int GetButtons(void)
{
	int i = 0;
	pinMode(BUTTON0_PIN, INPUT_PULLUP); // re-enable gpio in case it got disabled by standby mode
	pinMode(BUTTON1_PIN, INPUT_PULLUP);
	if (digitalRead(BUTTON0_PIN) == 0) i|=1;
	if (digitalRead(BUTTON1_PIN) == 0) i|=2;
	return i;

} /* GetButtons() */

void RunLowPower(void)
{
	int i, iUITick = 20, iSampleTick = 0;
	int bWasSuspended = 0;

    I2CSetSpeed(50000);
    scd41_start(SCD_POWERMODE_LOW); // start low power mode (available on SCD40 & SCD41)

	while (1) {
		i = GetButtons();
		if (i == 3) { // both buttons pressed, return to menu
			if (bWasSuspended == 1) {
				I2CInit(50000);
			}
			scd41_stop(); // stop collecting samples
			return;
		} else if (i && iUITick == 0) { // one button pressed, show the current data
			if (bWasSuspended == 1) {
				I2CInit(400000);
				bWasSuspended = 0;
			}
		   oledPower(1);
		   ShowCurrent(); // display the current conditions on the OLED
		   iUITick = 20; // number of 250ms periods before turning off the display
		}
#ifdef DEBUG_MODE
		Delay_Ms(250);
#else
	Standby82ms(3); // conserve power (1.8mA running, 10uA standby)
	bWasSuspended = 1;
#endif
		iSampleTick++;
		if (iSampleTick == 120) { // 30 seconds have passed
			if (bWasSuspended == 1) {
				I2CInit(50000);
				bWasSuspended = 0;
			} else {
				I2CSetSpeed(50000);
			}
	       scd41_getSample();
	       if(_iCO2 < 1000){ // show state by LEDs
               BlinkLED(LED_GREEN, 2);
           } else if(_iCO2 > 1000 && _iCO2 < 2000){
               BlinkLED(LED_GREEN, 2);
               BlinkLED(LED_RED, 3);
           } else {
               BlinkLED(LED_RED, 3);
           }
	       iSampleTick = 0; // restart the 30 second timer for the next sample
		}
		if (iUITick > 0) {
			iUITick--;
			if (iUITick == 0) { // shut off the display after 5 seconds
				if (bWasSuspended) {
					I2CInit(400000);
					bWasSuspended = 0;
				} else {
					I2CSetSpeed(400000);
				}
				oledPower(0);
			}
		}
	}
} /* RunLowPower() */

void RunStealth(void)
{
	int iTick=0, j, iLevel = 1;
	int iUpdate = state.iFreq * 4; // how many quarter seconds to update vibration result
  oledFill(0);
  oledWriteString(22,0,"Stealth", FONT_12x16, 0);
  oledWriteString(0,16,"CO2 measurements will", FONT_6x8, 0);
  oledWriteString(0,24,"be converted to 1-6", FONT_6x8, 0);
  oledWriteString(0,32,"pulses. 1=good, 6=bad", FONT_6x8, 0);
  oledWriteString(0,56,"press button to start", FONT_6x8, 0);
  while (GetButtons() != 0) {
	  Delay_Ms(20); // wait for user to release all buttons
  }
  while (j = GetButtons() == 0) {
	  Delay_Ms(20);
  }
  oledFill(0);
  oledPower(0);
  // start fast CO2 sampling
  I2CSetSpeed(50000);
  scd41_start(SCD_POWERMODE_NORMAL);

  while (1) {
	  j = GetButtons();
	  if (j == 3) { // return to menu
		  oledFill(0);
		  scd41_stop();
		  return;
	  }
	  Delay_Ms(250);
	  if ((iTick % 20) == 19) { // get new sample every 5 seconds
		  scd41_getSample();
		  iLevel = 1 + (_iCO2/500); // 0-499 = perfect, 500-999 = good, 1000-1499=so-so, 1500-1999=not great, 2000-2499=bad, 2500+ = very bad
		  if (iLevel < 1) iLevel = 1;
		  else if (iLevel > 6) iLevel = 6;
	  }
	  if ((iTick % iUpdate) == (iUpdate-1)) { // time to buzz
		  for (j=0; j<iLevel; j++) {
			  Vibrate(100);
			  Delay_Ms(395);
//			  BlinkLED(LED_GREEN, 5);
			  iTick += 2; // we delayed it 500ms
		  }
	  }
	  iTick++;
  } // while (1)
} /* RunStealth() */
#ifdef FUTURE
//
// Wait for user to press a button, show 1 minute of samples
// then go back to sleep
//
void RunOnDemand(void)
{
	Delay_Ms(2000); // show startup message for 2 seconds
	oledPower(0);
	while (1) {
		int i, j;
#ifdef DEBUG_MODE
			Delay_Ms(3*82); // use a power wasting delay to allow SWDIO to work
#else
			Standby82ms(3); // conserve power (1.8mA running, 10uA standby)
#endif
			i = GetButtons();
			if (i == 3) { // both buttons pressed
				return; // go back to main menu
			}
			if (i != 0) { // a button was pressed, display 1 minute of samples
               oledInit(0x3c, 400000);
			   oledFill(0);
			   oledWriteString(0,0,"Waking up...", FONT_8x8, 0);
			   I2CSetSpeed(50000);
		       scd41_start(SCD_POWERMODE_NORMAL);
			   for (j=0; j<4*60; j++) { // wait for time to pass
#ifdef DEBUG_MODE
				   Delay_Ms(250);
#else
				   Standby82ms(3);
#endif
				   if (j % 20 == 19) { // show new data every 5 seconds
					   I2CInit(50000);
					   scd41_getSample();
					   ShowCurrent(); // display the current conditions on the OLED
				   }
				   i = GetButtons();
				   if (i == 3) {
					   scd41_stop();
					   return; // go to main menu
				   }
			   } // for j (1 minute of samples
			   I2CInit(50000);
			   scd41_shutdown();
			   oledPower(0);
			} // a button was pressed
	} // while (1)
} /* RunOnDemand() */
#endif // FUTURE

void RunCalibrate(void)
{
	int i, j;

	oledFill(0);
	oledWriteString(10,0,"Calibrate", FONT_12x16, 0);
    oledWriteString(0,16,"Place device in a", FONT_6x8, 0);
    oledWriteString(0,24,"free air environment.", FONT_6x8, 0);
    oledWriteString(0,32,"Press either button", FONT_6x8, 0);
    oledWriteString(0,40,"to start. When timer", FONT_6x8, 0);
    oledWriteString(0,48,"finishes, result will", FONT_6x8, 0);
    oledWriteString(0,56,"show success or fail", FONT_6x8, 0);
    while (GetButtons()) {
    	Delay_Ms(20); // wait for user to release button(s)
    }
	while ((j = GetButtons()) == 0) {
		Delay_Ms(20);
	}
	if (j == 3) { // both buttons, exit
		return;
	}
	oledFill(0);
	oledWriteString(0,0,"Calibration running", FONT_6x8, 0);
   I2CSetSpeed(50000);
   scd41_start(SCD_POWERMODE_NORMAL);
   // allow 3 minutes of normal collection
   for (i=210; i>=0; i--) {
	  ShowTime(i);
	  j = GetButtons();
	  if (j == 3) { // user quit
		  scd41_stop();
		  return;
	  }
	  Delay_Ms(1000);
   }
   oledClearLine(24);
   oledClearLine(32);
   oledClearLine(40);
   oledClearLine(48);
   scd41_stop(); // stop periodic measurement
   i = scd41_recalibrate(423); // force recalibration
   if (i == SCD_SUCCESS)
	   oledWriteString(0,32, "Success!", FONT_12x16, 0);
   else
	   oledWriteString(0,32, "Failed", FONT_12x16, 0);
   oledWriteString(0,56, "Press button to exit", FONT_6x8, 0);
   while (GetButtons() == 0) {
	   Delay_Ms(20);
   }
} /* RunCalibrate() */

int main(void)
{
    int bWasSuspended = 0;
    Delay_Init();
    ReadFlash(); // get the user settings from FLASH
//    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);
//    Option_Byte_CFG(); // allow PD7 to be used as GPIO
//    Delay_Ms(5000); //100); // give time for power to settle
//    USART_Printf_Init(460800);
//    printf("SystemClk:%d\r\n",SystemCoreClock);
    pinMode(MOTOR_PIN, OUTPUT);
    digitalWrite(MOTOR_PIN, 0);
    state.iAlert = ALERT_LED;
    ShowAlert(); // blink LEDs
menu_top:
   RunMenu();
   // Display the chosen mode
	oledFill(0);
	oledWriteString(0,0,szMode[state.iMode], FONT_8x8, 0);
    oledWriteString(0,8,"Starting...", FONT_8x8, 0);
   if (state.iMode == MODE_TIMER) {
	   RunTimer();
	   goto menu_top;
   } else if (state.iMode == MODE_CALIBRATE) {
	   RunCalibrate();
	   goto menu_top;
   } else if (state.iMode == MODE_LOW_POWER) {
	   RunLowPower();
	   goto menu_top;
//   } else if (state.iMode == MODE_ON_DEMAND) {
//	   RunOnDemand();
//	   goto menu_top;
   } else if (state.iMode == MODE_STEALTH) {
	   RunStealth();
	   goto menu_top;
   } else { // continuous mode
	   I2CSetSpeed(50000);
	   scd41_start(SCD_POWERMODE_NORMAL);
#ifdef DEBUG_MODE
	   Delay_Ms(5000); // allow time for first sample to capture
#else
	   Standby82ms(59);
#endif
    while(1) {
    	int i, j;
        I2CInit(50000); // SCD40 can't handle 400k
    	//I2CSetSpeed(50000); // SCD40 can't handle 400k
    	scd41_getSample();
    	iSample++;
#ifdef FUTURE
    	if (iSample > 3) AddSample(iSample); // add it to collected stats
#endif // FUTURE
    	if (iSample == 16 && state.iMode != MODE_CONTINUOUS ) { // after 1 minute, turn off the display
    		oledPower(0); // turn off display
    	}
    	ShowCurrent(); // display the current conditions on the OLED
		for (i=0; i<61; i+= 3) { // 5 seconds total
#ifdef DEBUG_MODE
			Delay_Ms(3*82); // use a power wasting delay to allow SWDIO to work
#else
			Standby82ms(3); // conserve power (1.8mA running, 10uA standby)
			bWasSuspended = 1;
#endif
			j = GetButtons();
			if (j == 3) { // both buttons pressed
			    if (bWasSuspended == 1) {
			                    I2CInit(50000);
			                    bWasSuspended = 0;
			    }
 			    scd41_stop(); // stop periodic measurement
				goto menu_top;
			}
//				if (iMode == 0) {
					// OLED is off, turn it back on
//					I2CInit(100000);
//					oledPower(1);
//					iMode = 1;
//					while (digitalRead(BUTTON0_PIN) == 0) {}; // wait for button to release
//					iSample = 4; // reset sample counter to reset display timeout (does not affect sample collection)
//				} else {
//					ShowGraph(); // show collected stats
//					goto get_sample; // enough time has passed, get the next sample
//				}
//			}
		} // for i
    } // while (1)
   } // not timer mode
} /* main() */
