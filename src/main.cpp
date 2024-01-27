#if defined (__USE_LPCOPEN)
#if defined(NO_BOARD_LIB)
#include "chip.h"
#else
#include "board.h"
#endif
#endif

#include <cr_section_macros.h>
#include <cstdio>
#include <cstring>
#include "systick.h"
#include "LpcUart.h"
#include "esp8266_socket.h"
#include "retarget_uart.h"
#include "ModbusMaster.h"
#include "ModbusRegister.h"
#include "MQTTClient.h"
#include "DigitalIoPin.h"
#include "LiquidCrystal.h"
#include "I2C.h"
#include "stdarg.h"
#include "stdlib.h"
#include "parser.h"


#define SSID	    ""
#define PASSWORD    ""
#define BROKER_IP   "192.168.1.107"
#define BROKER_PORT  1883


/** 7-bit I2C addresses of Temperature Sensor */
#define I2C_PRESSURE_ADDR_7BIT  (0x40)

#define ALTITUDE_CORRECTION 0.95
#define SCALE_FACTOR 230
#define RUNNING_AVERAGE_COUNT 10.0

#define ERROR_TIMEOUT 300
#define OSCILLATION_DAMPENING 7

#define MAX_PASCALS 120
#define MIN_PASCALS 0

#define MAX_FANSPEED 1000
#define MIN_FANSPEED 0

#define SETTINGS_TOPIC "controller/settings"
#define STATUS_TOPIC "controller/status"

enum Mode { manual, automatic, debug };
int targetPascals = 50;
int fanSpeed = 0;
bool systemMode = false;
bool gotMsg = false;
bool input = false;
static volatile int counter;
static volatile uint32_t systicks;

#ifdef __cplusplus
extern "C" {
#endif
/**
 * @brief	Handle interrupt from SysTick timer
 * @return	Nothing
 */
void SysTick_Handler(void)
{
	systicks++;
	if(counter > 0) counter--;
}
uint32_t get_ticks(void){
	return systicks;
}
#ifdef __cplusplus
}
#endif

void Sleep(int ms)
{
	counter = ms;
	while(counter > 0) {
		__WFI();
	}
}

/* this function is required by the modbus library */
uint32_t millis(){
	return systicks;
}

// This function renders bars to make UI more clear on the physical ventilation screen
char* renderBar(int value, int maxValue, int length) {
	// We use malloc to allocate the string on the heap
	char* bars = (char*)malloc((length+1)*sizeof(char));
	int i = 0;
	for(i = 0; i < length; i++) bars[i] = '_';
	bars[i] = '\0';

	if(value <= 0 || maxValue <= 0) return bars;

	int barCount = ((float)value / (float)maxValue) * length;

	for(i = 0; i < length; i++) {
		if(i < barCount) bars[i] = '\xff';
	}
	bars[i] = '\0';

	return bars;
}

bool blockingBtnRead(DigitalIoPin* btn, int debounce_ms, int max_debounces) {
	if(btn->read()) {
		for(int i = 0; i < max_debounces && btn->read(); i++) {
			Sleep(debounce_ms);
		}
		return true;
	}
	return false;
}

void clampFanSpeed(int* fanSpeed) {
	if(*fanSpeed < MIN_FANSPEED) *fanSpeed = MIN_FANSPEED;
	if(*fanSpeed > MAX_FANSPEED) *fanSpeed = MAX_FANSPEED;
}

void messageArrived(MessageData* data){
	const char* msg = (const char*)data->message->payload+'\0';
	Parser parser;
	int result;
	result = parser.parseValue(msg, &systemMode, &fanSpeed, &targetPascals);
	gotMsg = true;
	input = true;
}

int main(void) {
	SystemCoreClockUpdate();
	Board_Init();
	Board_LED_Set(0, true);

	/* Enable and setup SysTick Timer at a periodic rate */
	SysTick_Config(SystemCoreClock / 1000);

	I2C_config cfg;
	I2C i2c(cfg);

	/* Disable the interrupt for the I2C */
	NVIC_DisableIRQ(I2C0_IRQn);

	DigitalIoPin *rs = new DigitalIoPin(0, 29, DigitalIoPin::output);
	DigitalIoPin *en = new DigitalIoPin(0, 9, DigitalIoPin::output);
	DigitalIoPin *d4 = new DigitalIoPin(0, 10, DigitalIoPin::output);
	DigitalIoPin *d5 = new DigitalIoPin(0, 16, DigitalIoPin::output);
	DigitalIoPin *d6 = new DigitalIoPin(1, 3, DigitalIoPin::output);
	DigitalIoPin *d7 = new DigitalIoPin(0, 0, DigitalIoPin::output);
	LiquidCrystal lcd(rs, en, d4, d5, d6, d7);

	DigitalIoPin modeBtn(0, 7, DigitalIoPin::pullup, true);
	DigitalIoPin leftBtn(1, 8, DigitalIoPin::pullup, true);
	DigitalIoPin rightBtn(0, 5, DigitalIoPin::pullup, true);

	// configure display geometry
	lcd.begin(16, 2);


	ModbusMaster fanNode(1);
	ModbusMaster co2Node(240);
	ModbusMaster tempAndHumidNode(241);

	fanNode.begin(9600);
	co2Node.begin(9600);
	tempAndHumidNode.begin(9600);

	ModbusRegister fanBus(&fanNode, 0);
	ModbusRegister temperatureBus(&tempAndHumidNode, 257, true);
	ModbusRegister co2Bus(&co2Node, 256, true);
	ModbusRegister humidityBus(&tempAndHumidNode, 256, true);

	Mode mode = debug;

	unsigned long long i = 0;
	unsigned long long errorTimer = 0;

	uint16_t temperature = 0;
	uint16_t co2 = 0;
	uint16_t humidity = 0;

	float pressure_avg = 0;
	bool pressure_avg_init = false;

	MQTTClient client;
	Network network;

	unsigned char sendbuf[256], readbuf[2556];
	int rc = 0;

	MQTTPacket_connectData connectData = MQTTPacket_connectData_initializer;

	NetworkInit(&network, SSID, PASSWORD);
	MQTTClientInit(&client, &network, 3000, sendbuf, sizeof(sendbuf), readbuf, sizeof(readbuf));

	char *address = (char*) BROKER_IP;
	rc = NetworkConnect(&network, address, BROKER_PORT);

	connectData.MQTTVersion = 3;
	connectData.clientID.cstring = (char*) "test";

	MQTTConnect(&client, &connectData);
	MQTTSubscribe(&client, SETTINGS_TOPIC, QOS2, messageArrived);

	int sampleNr = 0;
	bool error = false;

	while(true) {
		rc = MQTTSubscribe(&client, SETTINGS_TOPIC, QOS2, messageArrived);
		if (systemMode == false && gotMsg == true) {
			mode = manual;
			gotMsg = false;
		} else if (systemMode == true && gotMsg == true) {
			mode = automatic;
			gotMsg = false;
		}

		//Sleep between modbus reads, totaling 20ms
		temperature = temperatureBus.read();
		Sleep(7);
		co2 = co2Bus.read();
		Sleep(7);
		humidity = humidityBus.read();
		Sleep(6);

		struct pressureData {
			uint8_t data[2];
			uint8_t crc;
		} pressureData;

		uint8_t press_r_cmd = 0xf1;
		i2c.transaction(I2C_PRESSURE_ADDR_7BIT, &press_r_cmd, 1, (uint8_t*)&pressureData, 3);
		int16_t pressure = (pressureData.data[0] << 8) + pressureData.data[1];
		pressure = pressure * ALTITUDE_CORRECTION / SCALE_FACTOR;

		char lcdLine1[17] = "";
		char lcdLine2[17] = "";

		if(blockingBtnRead(&modeBtn, 10, 10000)) {
			if(mode == manual) mode = automatic;
			else if(mode == automatic) mode = debug;
			else if(mode == debug) mode = manual;
			input = true;
		}

		if(!pressure_avg_init) {
			pressure_avg = pressure;
			pressure_avg_init = true;
		}

		pressure_avg -= (float)pressure_avg / RUNNING_AVERAGE_COUNT;
		pressure_avg += (float)pressure / RUNNING_AVERAGE_COUNT;

		if(mode == automatic) {
			if(blockingBtnRead(&leftBtn, 10, 10)) targetPascals--, input = true;
			if(blockingBtnRead(&rightBtn, 10, 10)) targetPascals++, input = true;
			if(targetPascals < MIN_PASCALS) targetPascals = MIN_PASCALS;
			if(targetPascals > MAX_PASCALS) targetPascals = MAX_PASCALS;

			// If the delta is large we change the fan speed by a lot
			// Else we change it by less
			// This allows the system to be both responsive and stable
			int delta = targetPascals - pressure;
			int fanChange = delta / OSCILLATION_DAMPENING;
			if (fanChange == 0 && delta != 0) {
				if (delta < 0)
					fanChange = -1;
				if (delta > 0)
					fanChange = +1;
			}
			fanSpeed += fanChange;

			if(delta == 0) errorTimer = 0;
			error = errorTimer > ERROR_TIMEOUT;

			if(targetPascals <= MIN_PASCALS) fanSpeed = MIN_FANSPEED;

			clampFanSpeed(&fanSpeed);

			char* bars = renderBar(targetPascals, MAX_PASCALS, 14);
			snprintf(lcdLine1, 17, "%s:A", bars);
			snprintf(lcdLine2, 17, "%3.0fPa Goal:%3dPa", pressure_avg, targetPascals);
			free(bars);

			errorTimer++;
		}
		else if (mode == manual) {
			if(blockingBtnRead(&leftBtn, 10, 10)) fanSpeed-=10, input = true;
			if(blockingBtnRead(&rightBtn, 10, 10)) fanSpeed+=10, input = true;

			clampFanSpeed(&fanSpeed);

			char* bars = renderBar(fanSpeed, MAX_FANSPEED, 14);
			snprintf(lcdLine1, 17, "%s:M", bars);
			snprintf(lcdLine2, 17, "%3d%%rpm %3.0fPa", fanSpeed/10, pressure_avg);
			free(bars);

			errorTimer = 0;
			error = false;
		}
		else if (mode == debug) {
			snprintf(lcdLine1, 17, "%3d\xdf""C %3d%%RH       ", temperature/10, humidity/10);
			snprintf(lcdLine1, 17, "%.15sD", lcdLine1);
			snprintf(lcdLine2, 17, "%4dppm %3dPa", co2, pressure);

			errorTimer = 0;
			error = false;
		}


		fanBus.write(fanSpeed);

		lcd.clear();
		lcd.setCursor(0, 0);
		lcd.print(lcdLine1);
		lcd.setCursor(0, 1);
		lcd.print(lcdLine2);

		if(i % 50 == 0 || input == true){
			input = false;
			MQTTMessage message;
			char payload[128];

			message.qos = QOS1;
			message.retained = 0;
			message.payload = payload;
			sprintf(payload, "{\"sampleNr\":%d,\"fanSpeed\":%d,\"targetPa\":%d,\"pressurePa\":%d,\"co2ppm\":%d,\"rHumidity\":%.1f,\"tempCelsius\":%.1f,\"auto\": %d,\"error\":%d}",
					sampleNr,
					fanSpeed/10,
					targetPascals,
					pressure,
					co2,
					0.1*(float)humidity,
					0.1*(float)temperature,
					mode==automatic,
					error
			);
			message.payloadlen = strlen(payload);
			sampleNr++;


			MQTTPublish(&client, STATUS_TOPIC, &message);
		}

		i++;

	}


	// Enter an infinite loop, just incrementing a counter
	while(1) {
		// "Dummy" NOP to allow source level single
		// stepping of tight while() loop
		__asm volatile ("nop");
	}
	return 0 ;

}

