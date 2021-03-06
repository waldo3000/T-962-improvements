/*
 * main.c - T-962 reflow controller
 *
 * Copyright (C) 2014 Werner Johansson, wj@unifiedengineering.se
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "LPC214x.h"
#include <stdint.h>
#include <stdio.h>
#include "serial.h"
#include "lcd.h"
#include "io.h"
#include "sched.h"
#include "onewire.h"
#include "adc.h"
#include "i2c.h"
#include "rtc.h"
#include "eeprom.h"
#include "keypad.h"
#include "reflow.h"
#include "buzzer.h"
#include "nvstorage.h"
#include "version.h"
#include "vic.h"
#include "max31855.h"
#include "systemfan.h"

extern uint8_t logobmp[];
extern uint8_t stopbmp[];
extern uint8_t selectbmp[];
extern uint8_t editbmp[];
extern uint8_t f3editbmp[];

// No version.c file generated for LPCXpresso builds, fall back to this
__attribute__((weak)) const char* Version_GetGitVersion(void) {
    return "no version info";
}

// Support for boot ROM functions (get part number etc)
typedef void (*IAP)(unsigned int [],unsigned int[]);
IAP iap_entry = (void*)0x7ffffff1;
#define IAP_READ_PART     (54)
#define IAP_REINVOKE_ISP  (57)
#define PART_REV_ADDR (0x0007D070)
typedef struct {
	const char* name;
	const uint32_t id;
} partmapStruct;

partmapStruct partmap[] = {
		{"LPC2131(/01)", 0x0002ff01}, // Probably pointless but present for completeness (32kB flash is too small for factory image)
		{"LPC2132(/01)", 0x0002ff11},
		{"LPC2134(/01)", 0x0002ff12},
		{"LPC2136(/01)", 0x0002ff23},
		{"LPC2138(/01)", 0x0002ff25},

		{"LPC2141", 0x0402ff01}, // Probably pointless but present for completeness (32kB flash is too small for factory image)
		{"LPC2142", 0x0402ff11},
		{"LPC2144", 0x0402ff12},
		{"LPC2146", 0x0402ff23},
		{"LPC2148", 0x0402ff25},
};
#define NUM_PARTS (sizeof(partmap)/sizeof(partmap[0]))

uint32_t partid,partrev;
uint32_t command[1];
uint32_t result[3];

typedef struct {
	const char* formatstr;
	const NVItem_t nvval;
	const uint8_t minval;
	const uint8_t maxval;
	const int8_t offset;
	const float multiplier;
} setupMenuStruct;

setupMenuStruct setupmenu[] = {
		{"Min fan speed     %3.0f", REFLOW_MIN_FAN_SPEED, 0, 254, 0, 1.0f},
		{"Cycle done beep %4.1fs", REFLOW_BEEP_DONE_LEN, 0, 254, 0, 0.1f},
		{"Left TC gain     %1.2f", TC_LEFT_GAIN, 10, 190, 0, 0.01f},
		{"Left TC offset  %+1.2f", TC_LEFT_OFFSET, 0, 200, -100, 0.25f},
		{"Right TC gain    %1.2f", TC_RIGHT_GAIN, 10, 190, 0, 0.01f},
		{"Right TC offset %+1.2f", TC_RIGHT_OFFSET, 0, 200, -100, 0.25f},
};
#define NUM_SETUP_ITEMS (sizeof(setupmenu)/sizeof(setupmenu[0]))

static int32_t Main_Work( void );

int main(void) {
	char buf[22];
	int len;

	/* Hold F1-Key at boot to force ISP mode */
	if ((IOPIN0 & (1<<23)) == 0) {
		//NB: If you want to call this later need to set a bunch of registers back
		//    to reset state. Haven't fully figured this out yet, might want to
		//    progmatically call bootloader, not sure. If calling later be sure
		//    to crank up watchdog time-out, as it's impossible to disable
		//
		//    Bootloader must use legacy mode IO if you call this later too, so do:
		//    SCS = 0;

		//Turn off FAN & Heater using legacy registers so they stay off during bootloader
		//Fan = PIN0.8
		//Heater = PIN0.9
		IODIR0 = (1<<8) | (1<<9);
		IOSET0 = (1<<8) | (1<<9);

		//Re-enter ISP Mode, this function will never return
		command[0] = IAP_REINVOKE_ISP;
		iap_entry((void *)command, (void *)result);
	}

	PLLCFG = (1<<5) | (4<<0); //PLL MSEL=0x4 (+1), PSEL=0x1 (/2) so 11.0592*5 = 55.296MHz, Fcco = (2x55.296)*2 = 221MHz which is within 156 to 320MHz
	PLLCON = 0x01;
	PLLFEED = 0xaa;
	PLLFEED = 0x55; // Feed complete
	while(!(PLLSTAT & (1<<10))); // Wait for PLL to lock
	PLLCON = 0x03;
	PLLFEED = 0xaa;
	PLLFEED = 0x55; // Feed complete
	VPBDIV = 0x01; // APB runs at the same frequency as the CPU (55.296MHz)
	MAMTIM = 0x03; // 3 cycles flash access recommended >40MHz
	MAMCR = 0x02; // Fully enable memory accelerator

	VIC_Init();
	Sched_Init();
	IO_Init();
	Set_Heater(0);
	Set_Fan(0);
	Serial_Init();
	printf(	"\nT-962-controller open source firmware (%s)" \
			"\n" \
			"\nSee https://github.com/UnifiedEngineering/T-962-improvement for more details." \
			"\n" \
			"\nInitializing improved reflow oven...", Version_GetGitVersion());
	I2C_Init();
	EEPROM_Init();
	NV_Init();

	LCD_Init();
	LCD_BMPDisplay(logobmp,0,0);

	// Setup watchdog
	WDTC = PCLKFREQ / 3; // Some margin (PCLKFREQ/4 would be exactly the period the WD is fed by sleep_work)
	WDMOD = 0x03; // Enable
	WDFEED = 0xaa;
	WDFEED = 0x55;

	uint8_t resetreason = RSIR;
	RSIR = 0x0f; // Clear it out
	printf("\nReset reason(s): %s%s%s%s", (resetreason&(1<<0))?"[POR]":"", (resetreason&(1<<1))?"[EXTR]":"",
			(resetreason&(1<<2))?"[WDTR]":"", (resetreason&(1<<3))?"[BODR]":"");

	// Request part number
	command[0] = IAP_READ_PART;
	iap_entry((void *)command, (void *)result);
	const char* partstrptr = NULL;
	for(int i=0; i<NUM_PARTS; i++) {
		if(result[1] == partmap[i].id) {
			partstrptr = partmap[i].name;
			break;
		}
	}
	// Read part revision
	partrev=*(uint8_t*)PART_REV_ADDR;
	if(partrev==0 || partrev > 0x1a) {
		partrev = '-';
	} else {
		partrev += 'A' - 1;
	}
	len = snprintf(buf,sizeof(buf),"%s rev %c",partstrptr,(int)partrev);
	LCD_disp_str((uint8_t*)buf, len, 0, 64-6, FONT6X6);
	printf("\nRunning on an %s", buf);

	len = snprintf(buf,sizeof(buf),"%s",Version_GetGitVersion());
	LCD_disp_str((uint8_t*)buf, len, 128-(len*6), 0, FONT6X6);

	LCD_FB_Update();
	Keypad_Init();
	Buzzer_Init();
	ADC_Init();
	RTC_Init();
	OneWire_Init();
	SPI_TC_Init();
	Reflow_Init();
	SystemFan_Init();

	Sched_SetWorkfunc( MAIN_WORK, Main_Work );
	Sched_SetState( MAIN_WORK, 1, TICKS_SECS( 2 ) ); // Enable in 2 seconds

	Buzzer_Beep( BUZZ_1KHZ, 255, TICKS_MS(100) );

	while(1) {
#ifdef ENABLE_SLEEP
		int32_t sleeptime;
		sleeptime=Sched_Do( 0 ); // No fast-forward support
		//printf("\n%d ticks 'til next activity"),sleeptime);
#else
		Sched_Do( 0 ); // No fast-forward support
#endif
	}
	return 0;
}

typedef enum eMainMode {
	MAIN_HOME = 0,
	MAIN_ABOUT,
	MAIN_SETUP,
	MAIN_BAKE,
	MAIN_SELECT_PROFILE,
	MAIN_EDIT_PROFILE,
	MAIN_REFLOW
} MainMode_t;

static int32_t Main_Work(void) {
	static MainMode_t mode = MAIN_HOME;
	static uint32_t setpoint = 30;

	// profile editing
	static uint8_t profile_time_idx = 0;
	static uint8_t current_edit_profile;

	int32_t retval = TICKS_MS(500);

	char buf[22];
	int len;

	uint32_t keyspressed = Keypad_Get();

	// main menu state machine
	if (mode == MAIN_SETUP) {
		static uint8_t selected = 0;
		int y = 0;

		int keyrepeataccel = keyspressed >> 17; // Divide the value by 2
		if (keyrepeataccel < 1) keyrepeataccel = 1;
		if (keyrepeataccel > 30) keyrepeataccel = 30;

		if (keyspressed & KEY_F1) {
			if (selected > 0) { // Prev row
				selected--;
			} else { // wrap
				selected = NUM_SETUP_ITEMS - 1;
			}
		}
		if (keyspressed & KEY_F2) {
			if (selected < (NUM_SETUP_ITEMS - 1)) { // Next row
				selected++;
			} else { // wrap
				selected = 0;
			}
		}

		int curval = NV_GetConfig(setupmenu[selected].nvval);
		if (keyspressed & KEY_F3) { // Decrease value
			int minval = setupmenu[selected].minval;
			curval -= keyrepeataccel;
			if (curval < minval) curval = minval;
		}
		if (keyspressed & KEY_F4) { // Increase value
			int maxval = setupmenu[selected].maxval;
			curval += keyrepeataccel;
			if( curval > maxval ) curval = maxval;
		}
		if (keyspressed & (KEY_F3 | KEY_F4)) {
			NV_SetConfig(setupmenu[selected].nvval, curval);
			Reflow_ValidateNV();
		}

		LCD_FB_Clear();
		len = snprintf(buf, sizeof(buf), "Setup/calibration");
		LCD_disp_str((uint8_t*)buf, len, 64 - (len * 3), y, FONT6X6);
		y += 7;

		for (int i = 0; i < NUM_SETUP_ITEMS ; i++) {
			int intval = NV_GetConfig(setupmenu[i].nvval);
			intval += setupmenu[i].offset;
			float value = ((float)intval) * setupmenu[i].multiplier;
			len = snprintf(buf,sizeof(buf), setupmenu[i].formatstr, value);
			LCD_disp_str((uint8_t*)buf, len, 0, y, FONT6X6 | (selected == i) ? INVERT : 0);
			y += 7;
		}

		// buttons
		y = 64 - 7;
		LCD_disp_str((uint8_t*)" < ", 3, 0, y, FONT6X6 | INVERT);
		LCD_disp_str((uint8_t*)" > ", 3, 20, y, FONT6X6 | INVERT);
		LCD_disp_str((uint8_t*)" - ", 3, 45, y, FONT6X6 | INVERT);
		LCD_disp_str((uint8_t*)" + ", 3, 65, y, FONT6X6 | INVERT);
		LCD_disp_str((uint8_t*)" DONE ", 6, 91, y, FONT6X6 | INVERT);

		//LCD_BMPDisplay(stopbmp,127-17,0);

		if (keyspressed & KEY_S) { // Leave setup
			mode = MAIN_HOME;
			Reflow_SetMode(REFLOW_STANDBY);
			retval = 0; // Force immediate refresh
		}
	} else if (mode == MAIN_ABOUT) {
		LCD_FB_Clear();
		LCD_BMPDisplay(logobmp, 0, 0);

		len = snprintf(buf, sizeof(buf), "T-962 controller");
		LCD_disp_str((uint8_t*)buf, len, 64 - (len * 3), 0, FONT6X6);

		len = snprintf(buf, sizeof(buf), "%s", Version_GetGitVersion());
		LCD_disp_str((uint8_t*)buf, len, 64-(len*3), 64 - 6, FONT6X6);

		LCD_BMPDisplay(stopbmp, 127 - 17, 0);

		// Leave about with any key.
		if (keyspressed & KEY_ANY) {
			mode = MAIN_HOME;
			retval = 0; // Force immediate refresh
		}
	} else if (mode == MAIN_REFLOW) {
		uint32_t ticks = RTC_Read();
		//len = snprintf(buf,sizeof(buf),"seconds:%d",ticks);
		//LCD_disp_str((uint8_t*)buf, len, 13, 0, FONT6X6);
		len = snprintf(buf, sizeof(buf), "%03u", Reflow_GetSetpoint());
		LCD_disp_str((uint8_t*)"SET", 3, 110, 7, FONT6X6);
		LCD_disp_str((uint8_t*)buf, len, 110, 13, FONT6X6);

		len = snprintf(buf, sizeof(buf), "%03u", Reflow_GetActualTemp());
		LCD_disp_str((uint8_t*)"ACT", 3, 110, 20, FONT6X6);
		LCD_disp_str((uint8_t*)buf, len, 110, 26, FONT6X6);

		len = snprintf(buf,sizeof(buf),"%03u", (unsigned int)ticks);
		LCD_disp_str((uint8_t*)"RUN", 3, 110, 33, FONT6X6);
		LCD_disp_str((uint8_t*)buf, len, 110, 39, FONT6X6);
		if (Reflow_IsDone() || keyspressed & KEY_S) { // Abort reflow
			if (Reflow_IsDone()) {
				Buzzer_Beep(BUZZ_1KHZ, 255, TICKS_MS(100) * NV_GetConfig(REFLOW_BEEP_DONE_LEN));
			}
			mode = MAIN_HOME;
			Reflow_SetMode(REFLOW_STANDBY);
			retval = 0; // Force immediate refresh
		}

	} else if (mode == MAIN_SELECT_PROFILE) {
		int curprofile = Reflow_GetProfileIdx();

		LCD_FB_Clear();

		if (keyspressed & KEY_F1) { // Prev profile
			curprofile--;
		}
		if (keyspressed & KEY_F2) { // Next profile
			curprofile++;
		}
		Reflow_SelectProfileIdx(curprofile);
		Reflow_PlotProfile(-1);
		LCD_BMPDisplay(selectbmp, 127 - 17, 0);
		int eeidx = Reflow_GetEEProfileIdx();
		if (eeidx) { // Display edit button
			LCD_BMPDisplay(f3editbmp, 127 - 17, 29);
		}
		len = snprintf(buf, sizeof(buf), "%s", Reflow_GetProfileName());
		LCD_disp_str((uint8_t*)buf, len, 13, 0, FONT6X6);

		if (eeidx && keyspressed & KEY_F3) { // Edit ee profile
			mode = MAIN_EDIT_PROFILE;
			current_edit_profile = eeidx;
			retval = 0; // Force immediate refresh
		}
		if (keyspressed & KEY_S) { // Select current profile
			mode = MAIN_HOME;
			retval = 0; // Force immediate refresh
		}

	} else if (mode == MAIN_BAKE) {
		LCD_FB_Clear();
		LCD_disp_str((uint8_t*)"MANUAL/BAKE MODE", 16, 0, 0, FONT6X6);
		int keyrepeataccel = keyspressed >> 17; // Divide the value by 2
		if (keyrepeataccel < 1) keyrepeataccel = 1;
		if (keyrepeataccel > 30) keyrepeataccel = 30;

		if (keyspressed & KEY_F1) { // Setpoint-
			setpoint -= keyrepeataccel;
			if (setpoint<30) setpoint = 30;
		}
		if (keyspressed & KEY_F2) { // Setpoint+
			setpoint += keyrepeataccel;
			if (setpoint>300) setpoint = 300;
		}

		len = snprintf(buf, sizeof(buf),"- SETPOINT %u` +", (unsigned int)setpoint);
		LCD_disp_str((uint8_t*)buf, len, 64-(len*3), 10, FONT6X6);

		LCD_disp_str((uint8_t*)"F1", 2, 0, 10, FONT6X6 | INVERT);
		LCD_disp_str((uint8_t*)"F2", 2, 127-12, 10, FONT6X6 | INVERT);

		len = snprintf(buf, sizeof(buf), "ACTUAL %.1f`", Reflow_GetTempSensor(TC_AVERAGE));
		LCD_disp_str((uint8_t*)buf, len, 64 - (len * 3), 18, FONT6X6);

		len = snprintf(buf, sizeof(buf), "L %.1f`", Reflow_GetTempSensor(TC_LEFT));
		LCD_disp_str((uint8_t*)buf, len, 32 - (len * 3), 26, FONT6X6);

		len = snprintf(buf, sizeof(buf), "R %.1f`", Reflow_GetTempSensor(TC_RIGHT));
		LCD_disp_str((uint8_t*)buf, len, 96 - (len * 3), 26, FONT6X6);

		if (Reflow_IsTempSensorValid(TC_EXTRA1)) {
			len = snprintf(buf, sizeof(buf), "X1 %.1f`", Reflow_GetTempSensor(TC_EXTRA1));
			LCD_disp_str((uint8_t*)buf, len, 32 - (len * 3), 34, FONT6X6);
		}

		if (Reflow_IsTempSensorValid(TC_EXTRA2)) {
			len = snprintf(buf, sizeof(buf), "X2 %.1f`", Reflow_GetTempSensor(TC_EXTRA2));
			LCD_disp_str((uint8_t*)buf, len, 96 - (len * 3), 34, FONT6X6);
		}

		if (Reflow_IsTempSensorValid(TC_COLD_JUNCTION)) {
			len = snprintf(buf, sizeof(buf), "COLD-JUNCTION %.1f`", Reflow_GetTempSensor(TC_COLD_JUNCTION));
		} else {
			len = snprintf(buf, sizeof(buf), "NO COLD-JUNCTION TS!");
		}
		LCD_disp_str((uint8_t*)buf, len, 64 - (len * 3), 42, FONT6X6);

		LCD_BMPDisplay(stopbmp, 127 - 17, 0);

//		len = snprintf(buf,sizeof(buf),"heat=0x%02x fan=0x%02x",heat,fan);
//		LCD_disp_str((uint8_t*)buf, len, 0, 63-5, FONT6X6);

		// Add timer for bake at some point

		Reflow_SetSetpoint(setpoint);

		if (keyspressed & KEY_S) { // Abort bake
			mode = MAIN_HOME;
			Reflow_SetMode(REFLOW_STANDBY);
			retval = 0; // Force immediate refresh
		}

	} else if (mode == MAIN_EDIT_PROFILE) { // Edit ee1 or 2
		LCD_FB_Clear();
		int keyrepeataccel = keyspressed >> 17; // Divide the value by 2
		if (keyrepeataccel < 1) keyrepeataccel = 1;
		if (keyrepeataccel > 30) keyrepeataccel = 30;

		int16_t cursetpoint;
		Reflow_SelectEEProfileIdx(current_edit_profile);
		if (keyspressed & KEY_F1 && profile_time_idx > 0) { // Prev time
			profile_time_idx--;
		}
		if (keyspressed & KEY_F2 && profile_time_idx < 47) { // Next time
			profile_time_idx++;
		}
		cursetpoint = Reflow_GetSetpointAtIdx(profile_time_idx);

		if (keyspressed & KEY_F3) { // Decrease setpoint
			cursetpoint -= keyrepeataccel;
		}
		if (keyspressed & KEY_F4) { // Increase setpoint
			cursetpoint += keyrepeataccel;
		}
		if (cursetpoint < 0) cursetpoint = 0;
		if (cursetpoint > 300) cursetpoint = 300;
		Reflow_SetSetpointAtIdx(profile_time_idx, cursetpoint);

		Reflow_PlotProfile(profile_time_idx);
		LCD_BMPDisplay(editbmp, 127 - 17, 0);

		len = snprintf(buf, sizeof(buf), "%02u0s %03u`", profile_time_idx, cursetpoint);
		LCD_disp_str((uint8_t*)buf, len, 13, 0, FONT6X6);
		if (keyspressed & KEY_S) { // Done editing
			Reflow_SaveEEProfile();
			mode = MAIN_HOME;
			retval = 0; // Force immediate refresh
		}

	} else { // Main menu
		LCD_FB_Clear();

		len = snprintf(buf, sizeof(buf),"MAIN MENU");
		LCD_disp_str((uint8_t*)buf, len, 0, 6 * 0, FONT6X6);
		LCD_disp_str((uint8_t*)"F1", 2, 0, 8 * 1, FONT6X6 | INVERT);
		LCD_disp_str((uint8_t*)"ABOUT", 5, 14, 8*1, FONT6X6);
		LCD_disp_str((uint8_t*)"F2", 2, 0, 8 * 2, FONT6X6 | INVERT);
		LCD_disp_str((uint8_t*)"SETUP", 5, 14, 8 * 2, FONT6X6);
		LCD_disp_str((uint8_t*)"F3", 2, 0, 8 * 3, FONT6X6 | INVERT);
		LCD_disp_str((uint8_t*)"BAKE/MANUAL MODE", 16, 14, 8 * 3, FONT6X6);
		LCD_disp_str((uint8_t*)"F4", 2, 0, 8 * 4, FONT6X6 | INVERT);
		LCD_disp_str((uint8_t*)"SELECT PROFILE", 14, 14, 8 * 4, FONT6X6);
		LCD_disp_str((uint8_t*)"S", 1, 3, 8*5, FONT6X6 | INVERT);
		LCD_disp_str((uint8_t*)"RUN REFLOW PROFILE", 18, 14, 8 * 5, FONT6X6);

		len = snprintf(buf, sizeof(buf), "%s", Reflow_GetProfileName());
		LCD_disp_str((uint8_t*)buf, len, 64 - (len * 3), 8 * 6, FONT6X6 | INVERT);

		len = snprintf(buf,sizeof(buf), "OVEN TEMPERATURE %d`", Reflow_GetActualTemp());
		LCD_disp_str((uint8_t*)buf, len, 64 - (len * 3), 64 - 6, FONT6X6);

		// Make sure reflow complete beep is silenced when pressing any key
		if (keyspressed) {
			Buzzer_Beep(BUZZ_NONE, 0, 0);
		}

		if (keyspressed & KEY_F1) { // About
			mode = MAIN_ABOUT;
			retval = 0; // Force immediate refresh
		}
		if (keyspressed & KEY_F2) { // Setup/cal
			mode = MAIN_SETUP;
			Reflow_SetMode(REFLOW_STANDBYFAN);
			retval = 0; // Force immediate refresh
		}
		if (keyspressed & KEY_F3) { // Bake mode
			mode = MAIN_BAKE;
			Reflow_Init();
			Reflow_SetMode(REFLOW_BAKE);
			retval = 0; // Force immediate refresh
		}
		if (keyspressed & KEY_F4) { // Select profile
			mode = MAIN_SELECT_PROFILE;
			retval = 0; // Force immediate refresh
		}
		if (keyspressed & KEY_S) { // Start reflow
			mode = MAIN_REFLOW;
			LCD_FB_Clear();
			Reflow_Init();
			Reflow_PlotProfile(-1);
			LCD_BMPDisplay(stopbmp, 127 - 17, 0);
			len = snprintf(buf, sizeof(buf), "%s", Reflow_GetProfileName());
			LCD_disp_str((uint8_t*)buf, len, 13, 0, FONT6X6);
			Reflow_SetMode(REFLOW_REFLOW);
			retval = 0; // Force immediate refresh
		}
	}

	LCD_FB_Update();

	return retval;
}
