/*
 * IsisTRXVUdemo.c
 *
 *  Created on: 6 feb. 2015
 *      Author: malv
 */

#include "common.h"
#include "trxvu_frame_ready.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <at91/utility/exithandler.h>
#include <at91/commons.h>
#include <at91/utility/trace.h>
#include <at91/peripherals/cp15/cp15.h>

#include <hal/Utility/util.h>
#include <hal/Timing/WatchDogTimer.h>
#include <hal/Drivers/I2C.h>
#include <hal/Drivers/LED.h>
#include <hal/boolean.h>
#include <hal/errors.h>

#include <satellite-subsystems/IsisTRXVU.h>

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <satellite-subsystems/GomEPS.h>
#if USING_GOM_EPS == 1
#include <SatelliteSubsystems/GomEPS.h>
#include <GomEPSdemos.h>
#endif
#include "IsisSPv2demo.h"
#include <hcc/api_fat.h>
#include <hcc/api_hcc_mem.h>
#include <hcc/api_mdriver_atmel_mcipdc.h>

////General Variables
#define TX_UPBOUND				30
#define TIMEOUT_UPBOUND			10

#define SIZE_RXFRAME	30
#define SIZE_TXFRAME	235

static xTaskHandle watchdogKickTaskHandle = NULL;
static xSemaphoreHandle trxvuInterruptTrigger = NULL;

// Test Function
static Boolean softResetVUTest(void) {
	printf(
			"\r\n Soft Reset of both receiver and transmitter microcontrollers \r\n");
	print_error(IsisTrxvu_componentSoftReset(0, trxvu_rc));
	vTaskDelay(1 / portTICK_RATE_MS);
	print_error(IsisTrxvu_componentSoftReset(0, trxvu_tc));

	return TRUE;
}

static Boolean hardResetVUTest(void) {
	printf(
			"\r\n Hard Reset of both receiver and transmitter microcontrollers \r\n");
	print_error(IsisTrxvu_componentHardReset(0, trxvu_rc));
	vTaskDelay(1 / portTICK_RATE_MS);
	print_error(IsisTrxvu_componentHardReset(0, trxvu_tc));

	return TRUE;
}

static Boolean vutc_sendDefClSignTest(void) {
	//Buffers and variables definition
	unsigned char testBuffer1[10] = { 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
			0x38, 0x39, 0x40 };
	unsigned char txCounter = 0;
	unsigned char avalFrames = 0;
	unsigned int timeoutCounter = 0;

	while (txCounter < 5 && timeoutCounter < 5) {
		printf(
				"\r\n Transmission of single buffers with default callsign. AX25 Format. \r\n");
		print_error(
				IsisTrxvu_tcSendAX25DefClSign(0, testBuffer1, 10, &avalFrames));

		if ((avalFrames != 0) && (avalFrames != 255)) {
			printf("\r\n Number of frames in the buffer: %d  \r\n", avalFrames);
			txCounter++;
		} else {
			vTaskDelay(100 / portTICK_RATE_MS);
			timeoutCounter++;
		}
	}

	return TRUE;
}

static Boolean vutc_toggleIdleStateTest(void) {
	static Boolean toggle_flag = 0;

	if (toggle_flag) {
		print_error(IsisTrxvu_tcSetIdlestate(0, trxvu_idle_state_off));
		toggle_flag = FALSE;
	} else {
		print_error(IsisTrxvu_tcSetIdlestate(0, trxvu_idle_state_on));
		toggle_flag = TRUE;
	}

	return TRUE;
}

static Boolean vutc_setTxBitrate9600Test(void) {
	print_error(IsisTrxvu_tcSetAx25Bitrate(0, trxvu_bitrate_9600));

	return TRUE;
}

static Boolean vutc_setTxBitrate1200Test(void) {
	print_error(IsisTrxvu_tcSetAx25Bitrate(0, trxvu_bitrate_1200));

	return TRUE;
}

static Boolean vurc_getFrameCountTest(void) {
	unsigned short RxCounter = 0;
	unsigned int timeoutCounter = 0;

	while (timeoutCounter < 4 * TIMEOUT_UPBOUND) {
		print_error(IsisTrxvu_rcGetFrameCount(0, &RxCounter));

		timeoutCounter++;

		vTaskDelay(10 / portTICK_RATE_MS);
	}
	printf("\r\n There are currently %d frames waiting in the RX buffer \r\n",
			RxCounter);

	return TRUE;
}

static Boolean vurc_getFrameCmdTest(void) {
	unsigned short RxCounter = 0;
	unsigned int i = 0;
	unsigned char rxframebuffer[SIZE_RXFRAME] = { 0 };
	ISIStrxvuRxFrame rxFrameCmd = { 0, 0, 0, rxframebuffer };

	print_error(IsisTrxvu_rcGetFrameCount(0, &RxCounter));

	printf("\r\nThere are currently %d frames waiting in the RX buffer\r\n",
			RxCounter);

	while (RxCounter > 0) {
		print_error(IsisTrxvu_rcGetCommandFrame(0, &rxFrameCmd));

		printf("Size of the frame is = %d \r\n", rxFrameCmd.rx_length);

		printf("Frequency offset (Doppler) for received frame: %.2f Hz\r\n",
				((double) rxFrameCmd.rx_doppler) * 13.352 - 22300.0); // Only valid for rev. B & C boards
		printf(
				"Received signal strength (RSSI) for received frame: %.2f dBm\r\n",
				((double) rxFrameCmd.rx_rssi) * 0.03 - 152);

		printf("The received frame content is = ");

		for (i = 0; i < rxFrameCmd.rx_length; i++) {
			printf("%02x ", rxFrameCmd.rx_framedata[i]);
		}
		printf("\r\n");

		print_error(IsisTrxvu_rcGetFrameCount(0, &RxCounter));

		vTaskDelay(10 / portTICK_RATE_MS);
	}

	return TRUE;
}

static Boolean vurc_getFrameCmdAndTxTest(void) {
	unsigned short RxCounter = 0;
	unsigned int i = 0;
	unsigned char rxframebuffer[SIZE_RXFRAME] = { 0 };
	unsigned char trxvuBuffer = 0;
	ISIStrxvuRxFrame rxFrameCmd = { 0, 0, 0, rxframebuffer };

	print_error(IsisTrxvu_rcGetFrameCount(0, &RxCounter));

	printf("\r\nThere are currently %d frames waiting in the RX buffer\r\n",
			RxCounter);

	while (RxCounter > 0) {
		print_error(IsisTrxvu_rcGetCommandFrame(0, &rxFrameCmd));

		printf("Size of the frame is = %d \r\n", rxFrameCmd.rx_length);

		printf("Frequency offset (Doppler) for received frame: %.2f Hz\r\n",
				((double) rxFrameCmd.rx_doppler) * 13.352 - 22300.0); // Only valid for rev. B & C boards
		printf(
				"Received signal strength (RSSI) for received frame: %.2f dBm\r\n",
				((double) rxFrameCmd.rx_rssi) * 0.03 - 152);

		rxframebuffer[26] = '-';
		rxframebuffer[27] = 'O';
		rxframebuffer[28] = 'B';
		rxframebuffer[29] = 'C';

		IsisTrxvu_tcSendAX25DefClSign(0, rxframebuffer, SIZE_RXFRAME,
				&trxvuBuffer);

		printf("The received frame content is = ");

		for (i = 0; i < rxFrameCmd.rx_length; i++) {
			printf("%02x ", rxFrameCmd.rx_framedata[i]);
		}
		printf("\r\n");

		print_error(IsisTrxvu_rcGetFrameCount(0, &RxCounter));

		vTaskDelay(10 / portTICK_RATE_MS);
	}

	return TRUE;
}

static void vurc_revDInterruptCallback() {
	portBASE_TYPE higherPriorityTaskWoken = pdFALSE;
	//Because the callback is from an interrupt context, we need to use FromISR
	xSemaphoreGiveFromISR(trxvuInterruptTrigger, &higherPriorityTaskWoken);
	//This forces a context switch to a now woken task
	// This should improve the response time for incoming packets
	if (higherPriorityTaskWoken == pdTRUE) {
		portYIELD_FROM_ISR();
	}
}

static Boolean vurc_getFrameCmdInterruptTest(void) {
	//Using a binary semaphore for syncronization between the interrupt and this task
	vSemaphoreCreateBinary(trxvuInterruptTrigger);
	//Create will do a give by itself. We need to counteract that
	xSemaphoreTake(trxvuInterruptTrigger, 0);
	//Enable the interrupt by giving it the callback we want to use
	TRXVU_FR_Enable(vurc_revDInterruptCallback);

	unsigned short RxCounter = 0;
	unsigned int i = 0;
	unsigned char rxframebuffer[SIZE_RXFRAME] = { 0 };
	ISIStrxvuRxFrame rxFrameCmd = { 0, 0, 0, rxframebuffer };
	Boolean continueRunning = TRUE;

	//Using a do-while loop here to ensure we empty the TRXVU of frames before
	// we wait for interrupts
	do {
		printf("\r\nFrame ready pin = %d\r\n", TRXVU_FR_IsReady());

		print_error(IsisTrxvu_rcGetFrameCount(0, &RxCounter));

		printf("There are currently %d frames waiting in the RX buffer\r\n",
				RxCounter);

		for (; RxCounter > 0; RxCounter--) {
			print_error(IsisTrxvu_rcGetCommandFrame(0, &rxFrameCmd));

			printf("Size of the frame is = %d \r\n", rxFrameCmd.rx_length);

			printf("Frequency offset (Doppler) for received frame: %.2f Hz\r\n",
					((double) rxFrameCmd.rx_doppler) * 13.352 - 22300.0); // Only valid for rev. B & C boards
			printf(
					"Received signal strength (RSSI) for received frame: %.2f dBm\r\n",
					((double) rxFrameCmd.rx_rssi) * 0.03 - 152);

			printf("The received frame content is = ");

			for (i = 0; i < rxFrameCmd.rx_length; i++) {
				printf("%02x ", rxFrameCmd.rx_framedata[i]);
			}
			printf("\r\n");
		}
		//Ensure some time is passing after last frame retrieve to ensure
		// the frame ready pin has sufficient time to go low
		// The wait requirement is about 0.5ms. To ensure that we actually need
		// to use 2ms as the argument here. That will result in 1-2ms wait
		vTaskDelay(2 / portTICK_RATE_MS);

		//In some cases, new frames arriving while processing the existing ones
		// This will essentially emulate the interrupt occurring again
		if (TRXVU_FR_IsReady()) {
			xSemaphoreGive(trxvuInterruptTrigger);
		}

		//Check for a new interrupt, and letting it timeout every 10th second.
		// Blocking using portMAX_DELAY is possible, but it is not recommended
		// to rely solely on the pin and interrupts. For demo purposes we do
		// however rely on the pin and the interrupts, and instead check for
		// user input in order to be able to escape the demo
		printf(
				"Press ESC if you want to stop the loop (you may have to wait 10 second)\r\n");
		do {
			if (DBGU_IsRxReady() && DBGU_GetChar() == 0x1B) {
				continueRunning = FALSE;
				printf("Exiting the demo\r\n");
			}
		} while (continueRunning && xSemaphoreTake(trxvuInterruptTrigger,
				10000 / portTICK_RATE_MS) == pdFALSE);
	} while (continueRunning);

	TRXVU_FR_Disable();
	vSemaphoreDelete(trxvuInterruptTrigger);

	return TRUE;
}

static Boolean vurc_getRxTelemTest_revD(void) {
	unsigned short telemetryValue;
	float eng_value = 0.0;
	ISIStrxvuRxTelemetry telemetry;
	int rv;

	// Telemetry values are presented as raw values
	printf("\r\nGet all Telemetry at once in raw values \r\n\r\n");
	rv = IsisTrxvu_rcGetTelemetryAll(0, &telemetry);
	if (rv) {
		printf("Subsystem call failed. rv = %d", rv);
		return TRUE;
	}

	telemetryValue = telemetry.fields.rx_doppler;
	eng_value = ((float) telemetryValue) * 13.352 - 22300;
	printf("Receiver doppler = %f Hz\r\n", eng_value);

	telemetryValue = telemetry.fields.rx_rssi;
	eng_value = ((float) telemetryValue) * 0.03 - 152;
	printf("Receiver RSSI = %f dBm\r\n", eng_value);

	telemetryValue = telemetry.fields.bus_volt;
	eng_value = ((float) telemetryValue) * 0.00488;
	printf("Bus voltage = %f V\r\n", eng_value);

	telemetryValue = telemetry.fields.vutotal_curr;
	eng_value = ((float) telemetryValue) * 0.16643964;
	printf("Total current = %f mA\r\n", eng_value);

	telemetryValue = telemetry.fields.vutx_curr;
	eng_value = ((float) telemetryValue) * 0.16643964;
	printf("Transmitter current = %f mA\r\n", eng_value);

	telemetryValue = telemetry.fields.vurx_curr;
	eng_value = ((float) telemetryValue) * 0.16643964;
	printf("Receiver current = %f mA\r\n", eng_value);

	telemetryValue = telemetry.fields.vupa_curr;
	eng_value = ((float) telemetryValue) * 0.16643964;
	printf("PA current = %f mA\r\n", eng_value);

	telemetryValue = telemetry.fields.pa_temp;
	eng_value = ((float) telemetryValue) * -0.07669 + 195.6037;
	printf("PA temperature = %f degC\r\n", eng_value);

	telemetryValue = telemetry.fields.board_temp;
	eng_value = ((float) telemetryValue) * -0.07669 + 195.6037;
	printf("Local oscillator temperature = %f degC\r\n", eng_value);

	return TRUE;
}

static Boolean vutc_getTxTelemTest_revD(void) {
	unsigned short telemetryValue;
	float eng_value = 0.0;
	ISIStrxvuTxTelemetry telemetry;
	int rv;

	// Telemetry values are presented as raw values
	printf("\r\nGet all Telemetry at once in raw values \r\n\r\n");
	rv = IsisTrxvu_tcGetTelemetryAll(0, &telemetry);
	if (rv) {
		printf("Subsystem call failed. rv = %d", rv);
		return TRUE;
	}

	telemetryValue = telemetry.fields.tx_reflpwr;
	eng_value = ((float) (telemetryValue * telemetryValue)) * 5.887E-5;
	printf("RF reflected power = %f mW\r\n", eng_value);

	telemetryValue = telemetry.fields.tx_fwrdpwr;
	eng_value = ((float) (telemetryValue * telemetryValue)) * 5.887E-5;
	printf("RF forward power = %f mW\r\n", eng_value);

	telemetryValue = telemetry.fields.bus_volt;
	eng_value = ((float) telemetryValue) * 0.00488;
	printf("Bus voltage = %f V\r\n", eng_value);

	telemetryValue = telemetry.fields.vutotal_curr;
	eng_value = ((float) telemetryValue) * 0.16643964;
	printf("Total current = %f mA\r\n", eng_value);

	telemetryValue = telemetry.fields.vutx_curr;
	eng_value = ((float) telemetryValue) * 0.16643964;
	printf("Transmitter current = %f mA\r\n", eng_value);

	telemetryValue = telemetry.fields.vurx_curr;
	eng_value = ((float) telemetryValue) * 0.16643964;
	printf("Receiver current = %f mA\r\n", eng_value);

	telemetryValue = telemetry.fields.vupa_curr;
	eng_value = ((float) telemetryValue) * 0.16643964;
	printf("PA current = %f mA\r\n", eng_value);

	telemetryValue = telemetry.fields.pa_temp;
	eng_value = ((float) telemetryValue) * -0.07669 + 195.6037;
	printf("PA temperature = %f degC\r\n", eng_value);

	telemetryValue = telemetry.fields.board_temp;
	eng_value = ((float) telemetryValue) * -0.07669 + 195.6037;
	printf("Local oscillator temperature = %f degC\r\n", eng_value);

	return TRUE;
}

static Boolean vutc_sendPacketInsertedByTheUser(void) {
	unsigned char testBuffer[5];
	int amountOfRepetitions;
	unsigned char txCounter = 0;
	unsigned char avalFrames = 0;
	int timeoutCounter = 0;
	int i;
	unsigned int temp;
	printf("\r\nEnter Hexa buffer: \r\n");
	for (i = 0; i < 5; i++) {
		if (UTIL_DbguGetHexa32(&temp) == 1)
			testBuffer[i] = (unsigned char) temp;
		else
			i--;
	}
	printf("\r\nEnter amount of repetitions: \r\n");
	while (UTIL_DbguGetIntegerMinMax(&amountOfRepetitions, 1, 1000) == 0)
		;
	while (txCounter < amountOfRepetitions
			&& timeoutCounter < amountOfRepetitions) {
		printf(
				"\r\n Transmission of single buffers with default callsign. AX25 Format. \r\n");
		print_error(
				IsisTrxvu_tcSendAX25DefClSign(0, testBuffer, 5, &avalFrames));
		if ((avalFrames != 0) && (avalFrames != 255)) {
			printf("\r\n Number of frames in the buffer: %d  \r\n", avalFrames);
			txCounter++;
		} else {
			vTaskDelay(100 / portTICK_RATE_MS);
			timeoutCounter++;
		}
	}

	return TRUE;
}

/*
 * this function is to init the SD card, it called in print_beacon_test function
 */
Boolean initSD() {
	int err;
	err = hcc_mem_init();
	if (err) {
		return FALSE;
	}

	err = fs_init();
	if (err) {
		return FALSE;
	}

	err = fs_start();
	if (err) {
		return FALSE;
	}

	err = f_enterFS();
	if (err) {
		return FALSE;
	}

	err = f_initvolume(0, atmel_mcipdc_initfunc, 0);
	if (err) {
		return FALSE;
	}

	return TRUE;
}

/*
 * print beacon test
 * printing: voltage of the battery, Temp of battery and solar panels
 * printing: (zerem) current out of the battery
 * printing: the usage of the SD card
 */

static Boolean print_beacon_test(void) {
	if (GomEPSdemoInit()) {
		initSPv2();

		gom_eps_hk_t myEpsTelemetry_hk;

		printf("\r\nEPS Telemetry HK General \r\n\n");
		print_error(GomEpsGetHkData_general(0, &myEpsTelemetry_hk));
		printf("Voltage of the battery = %d mV\r\n",
				myEpsTelemetry_hk.fields.vbatt);
		printf("Temperature sensors. TEMP1 = %d \r\n",
				myEpsTelemetry_hk.fields.temp[0]);
		printf("Temperature sensors. TEMP2 = %d \r\n",
				myEpsTelemetry_hk.fields.temp[1]);
		printf("Temperature sensors. TEMP3 = %d \r\n",
				myEpsTelemetry_hk.fields.temp[2]);
		printf("Temperature sensors. TEMP4 = %d \r\n",
				myEpsTelemetry_hk.fields.temp[3]);
		printf("Temperature sensors. BATT0 = %d \r\n",
				myEpsTelemetry_hk.fields.temp[4]);
		printf("Temperature sensors. BATT1 = %d \r\n",
				myEpsTelemetry_hk.fields.temp[5]);
		printf("Current out of the battery = %d mA\r\n",
				myEpsTelemetry_hk.fields.cursys);
		SolarPanelv2_Temperature();

		if (initSD()) {
			F_SPACE space;
			/* get free space on current drive */
			int ret = f_getfreespace(f_getdrive(), &space);
			if (!ret) {
				printf(
						"\nSD Card space:\n %d bytes total,\n %d bytes free,\n %d bytes used,\n %d bytes bad. \r\n",
						space.total, space.free, space.used, space.bad);
			} else {
				printf("\nError %d reading drive\n", ret);
			}
		}
	}
	return TRUE;
}

/*
 *beacon test with Ax25 message
 */
static Boolean beacon_Ax25_test(void) {
	static unsigned char fromCallSign[7] = { 'A', 'H', 'V', 'A', '1', '0', 0 };
	static unsigned char toCallSign[7] = { 'Y', 'A', 'R', 'K', 'A', '1', 0 };
	static unsigned char data[80] = "";
	//CubeSat Yarka is in air .. say hi to all universe .....
	printf("TRXVU Beacon Ax.25 test:\r\n");
	printf("write your message:\r\n");
	INPUT_GetSTRING("Beacon Message (max 80 chars): ", data, sizeof(data));
	unsigned short interval = INPUT_GetUINT16("Beacon interval in seconds: ");
	int r = IsisTrxvu_tcSetAx25BeaconOvrClSign(0, fromCallSign, toCallSign,
			data, sizeof(data), interval);
	if(r==0)
		printf("Beacon sent!");
	print_error(r);
	return TRUE;
}

static Boolean clear_beacon_Ax25_test(void) {
	int r = IsisTrxvu_tcClearBeacon(0);
	if(r==0)
		printf("Beacon Ax25 Cleared");
	print_error(r);
	return TRUE;
}

static Boolean TurnOnTransponderWithDelay(void) {
	printf("pick time in minutes for Turning ON the transporder: \n\r");
	unsigned char turn_on_cmd[] = { 0x38, 2 };
	I2C_write(0x61, turn_on_cmd, 2);

	int time_in_min = 0;
	while (UTIL_DbguGetIntegerMinMax(&time_in_min, 1, 20) == 0)
		;

	vTaskDelay(time_in_min * (60000 / portTICK_RATE_MS)); // ms: s 1000:1 ...

	unsigned char turn_off_cmd[] = { 0x38, 1 };
	I2C_write(0x61, turn_off_cmd, 2);

	return TRUE;
}

static Boolean TurnOnTransponder(void) {
	unsigned char turn_on_cmd[] = { 0x38, 2 };
	int* r =I2C_write(0x61, turn_on_cmd, 2);
	if(r==0)
		printf("Transponder is ON \n\r");
	else printf("Error No': {%d} turning transporder ON \n\r",r);

	return TRUE;
}

static Boolean TurnOffTransponder(void) {
	unsigned char turn_off_cmd[] = { 0x38, 1 };
	int* r =I2C_write(0x61, turn_off_cmd, 2);
	if(r==0)
		printf("Transponder is OFF \n\r");
	else printf("Error No': {%d} turning transporder OFF \n\r",r);
	return TRUE;
}

static void TransponderTimerTask(void *parameters) {
	int time_in_min = (int) parameters;
	vTaskDelay(time_in_min * (60000 / portTICK_RATE_MS)); // ms: s 1000:1 ....
	unsigned char turn_off_cmd[] = { 0x38, 1 };
	I2C_write(0x61, turn_off_cmd, 2);
}

static Boolean turnOnTransponderWithTask(void) {
	unsigned char turn_on_cmd[] = { 0x38, 2 };
	I2C_write(0x61, turn_on_cmd, 2);
	int time_in_min = 1;
	while (UTIL_DbguGetIntegerMinMax(&time_in_min, 1, 20) == 0)
		;
	xTaskCreate(TransponderTimerTask, (signed char* )"TransponderTimerTask",
			2048, (void * )time_in_min, tskIDLE_PRIORITY, NULL);
	return TRUE;
}

static Boolean selectAndExecuteTRXVUDemoTest(void) {
	int selection = 0;
	Boolean offerMoreTests = TRUE;

	printf("\n\r Select a test to perform: \n\r");
	printf("\t 1) Soft Reset TRXVU both microcontrollers \n\r");
	printf("\t 2) Hard Reset TRXVU both microcontrollers \n\r");
	printf("\t 3) Default Callsign Send Test\n\r");
	printf("\t 4) Toggle Idle state \n\r");
	printf("\t 5) Change transmission bitrate to 9600  \n\r");
	printf("\t 6) Change transmission bitrate to 1200 \n\r");
	printf("\t 7) Get frame count \n\r");
	printf("\t 8) Get command frame \n\r");
	printf("\t 9) Get command frame and retransmit \n\r");
	printf("\t 10) (revD) Get command frame by interrupt \n\r");
	printf("\t 11) (revD) Get receiver telemetry \n\r");
	printf("\t 12) (revD) Get transmitter telemetry \n\r");
	printf("\t 13) Turn On Transponder With Delay \n\r");
	printf("\t 14) Turn On Transponder \n\r");
	printf("\t 15) Turn Off Transponder \n\r");
	printf("\t 16) turn On Transponder With Task \n\r");
	printf("\t 17) Send Packet Inserted By The User \n\r");
	printf("\t 18) Beacon Ax.25 test \n\r");
	printf("\t 19) Clear the Ax.25 Beacon test\n\r");
	printf("\t 20) Print Beacon test \n\r");
	printf("\t 21) Return to main menu \n\r");

	while (UTIL_DbguGetIntegerMinMax(&selection, 1, 21) == 0)
		;

	switch (selection) {
	case 1:
		offerMoreTests = softResetVUTest();
		break;
	case 2:
		offerMoreTests = hardResetVUTest();
		break;
	case 3:
		offerMoreTests = vutc_sendDefClSignTest();
		break;
	case 4:
		offerMoreTests = vutc_toggleIdleStateTest();
		break;
	case 5:
		offerMoreTests = vutc_setTxBitrate9600Test();
		break;
	case 6:
		offerMoreTests = vutc_setTxBitrate1200Test();
		break;
	case 7:
		offerMoreTests = vurc_getFrameCountTest();
		break;
	case 8:
		offerMoreTests = vurc_getFrameCmdTest();
		break;
	case 9:
		offerMoreTests = vurc_getFrameCmdAndTxTest();
		break;
	case 10:
		offerMoreTests = vurc_getFrameCmdInterruptTest();
		break;
	case 11:
		offerMoreTests = vurc_getRxTelemTest_revD();
		break;
	case 12:
		offerMoreTests = vutc_getTxTelemTest_revD();
		break;
	case 13:
		offerMoreTests = TurnOnTransponderWithDelay();
		break;
	case 14:
		offerMoreTests = TurnOnTransponder();
		break;
	case 15:
		offerMoreTests = TurnOffTransponder();
		break;
	case 16:
		offerMoreTests = turnOnTransponderWithTask();
		break;
	case 17:
		offerMoreTests = vutc_sendPacketInsertedByTheUser();
		break;
	case 18:
		offerMoreTests = beacon_Ax25_test();
		break;
	case 19:
		offerMoreTests = clear_beacon_Ax25_test();
		break;
	case 20:
		offerMoreTests = print_beacon_test();
		break;
	case 21:
		offerMoreTests = FALSE;
		break;
	default:
		break;
	}

	return offerMoreTests;
}

static void _WatchDogKickTask(void *parameters) {
	(void) parameters;
	// Kick radio I2C watchdog by requesting uptime every 10 seconds
	portTickType xLastWakeTime = xTaskGetTickCount();
	for (;;) {
		unsigned int uptime;
		(void) IsisTrxvu_tcGetUptime(0, &uptime);
		vTaskDelayUntil(&xLastWakeTime, 10000);
	}
}

Boolean IsisTRXVUdemoInit(void) {
	// Definition of I2C and TRXUV
	ISIStrxvuI2CAddress myTRXVUAddress[1];
	ISIStrxvuFrameLengths myTRXVUBuffers[1];
	ISIStrxvuBitrate myTRXVUBitrates[1];
	int rv;

	//I2C addresses defined
	myTRXVUAddress[0].addressVu_rc = 0x60;
	myTRXVUAddress[0].addressVu_tc = 0x61;

	//Buffer definition
	myTRXVUBuffers[0].maxAX25frameLengthTX = SIZE_TXFRAME;
	myTRXVUBuffers[0].maxAX25frameLengthRX = SIZE_RXFRAME;

	//Bitrate definition
	myTRXVUBitrates[0] = trxvu_bitrate_1200;

	//Initialize the trxvu subsystem
	rv = IsisTrxvu_initialize(myTRXVUAddress, myTRXVUBuffers, myTRXVUBitrates,
			1);
	if (rv != E_NO_SS_ERR && rv != E_IS_INITIALIZED) {
		// we have a problem. Indicate the error. But we'll gracefully exit to the higher menu instead of
		// hanging the code
		TRACE_ERROR(
				"\n\r IsisTrxvu_initialize() failed; err=%d! Exiting ... \n\r",
				rv);
		return FALSE;
	}

	// Start watchdog kick task
	xTaskCreate(_WatchDogKickTask, (signed char* )"WDT", 2048, NULL,
			tskIDLE_PRIORITY, &watchdogKickTaskHandle);

	return TRUE;
}

void IsisTRXVUdemoLoop(void) {
	Boolean offerMoreTests = FALSE;

	while (1) {
		offerMoreTests = selectAndExecuteTRXVUDemoTest();// show the demo command line interface and handle commands

		if (offerMoreTests == FALSE)				// was exit/back selected?
		{
			break;
		}
	}
}

Boolean IsisTRXVUdemoMain(void) {
	if (IsisTRXVUdemoInit())// initialize of I2C and IsisTRXVU subsystem drivers succeeded?
	{
		IsisTRXVUdemoLoop();// show the main IsisTRXVU demo interface and wait for user input
		return TRUE;
	} else {
		return FALSE;
	}
}

Boolean TRXVUtest(void) {
	IsisTRXVUdemoMain();
	return TRUE;
}
