#include "key.h"
#include "stm32f4xx_hal.h"
#include <stdint.h>
#include "boot_manager.h"

/* 按键配置表 (定义在此处避免头文件多重定义) */
KeyInfo keys[] = {
	{
		.gpioPort = KEY_0_GPIO_Port,
		.gpioPin = KEY_0_Pin,
		.state = IDLE_STATE,
		.pressStartTime = 0,
		.releaseTime = 0,
		.lastLongAddTime = 0,
		.onShortPress = Key0_ShortPress_Callback,
		.onLongPress = Key0_LongPress_Callback,
		.onDoublePress = Key0_DoublePress_Callback,
		.keyName = "Key0"
	},
	{
		.gpioPort = KEY_1_GPIO_Port,
		.gpioPin = KEY_1_Pin,
		.state = IDLE_STATE,
		.pressStartTime = 0,
		.releaseTime = 0,
		.lastLongAddTime = 0,
		.onShortPress = Key1_ShortPress_Callback,
		.onLongPress = Key1_LongPress_Callback,
		.onDoublePress = Key1_DoublePress_Callback,
		.keyName = "Key1"
	},
	{
		.gpioPort = KEY_2_GPIO_Port,
		.gpioPin = KEY_2_Pin,
		.state = IDLE_STATE,
		.pressStartTime = 0,
		.releaseTime = 0,
		.lastLongAddTime = 0,
		.onShortPress = Key2_ShortPress_Callback,
		.onLongPress = Key2_LongPress_Callback,
		.onDoublePress = Key2_DoublePress_Callback,
		.keyName = "Key2"
	}
};
const uint32_t key_count = sizeof(keys) / sizeof(KeyInfo);

// key0回调
void Key0_ShortPress_Callback(void) {
	boot_manager_request_sd_upgrade();
}
void Key0_LongPress_Callback(void) {

}
void Key0_DoublePress_Callback(void) {

}

// key1回调
void Key1_ShortPress_Callback(void) {
	boot_manager_request_uart_upgrade();
}
void Key1_LongPress_Callback(void) {

}
void Key1_DoublePress_Callback(void) {

}

// key2回调
void Key2_ShortPress_Callback(void) {

}
void Key2_LongPress_Callback(void) {
    /* 长按 Key2 进入串口升级模式, 防止误触 */
    boot_manager_request_uart_upgrade();
}
void Key2_DoublePress_Callback(void) {

}

// 按键代码
void key_handle(void) {
	for (int i = 0; i < KEY_COUNT; i++) {
		KeyInfo* key = &keys[i];  
		int keyStatus = HAL_GPIO_ReadPin(key->gpioPort, key->gpioPin); 
		uint16_t currentTime = HAL_GetTick();

		switch (key->state) {
			case IDLE_STATE:
			if (keyStatus == 0) { 
				key->state = PRESS_DETECTED_STATE;
				key->pressStartTime = currentTime;
				key->lastLongAddTime = currentTime;
			}
			break;

			case PRESS_DETECTED_STATE:
			if (keyStatus == 1) { 
				key->releaseTime = currentTime;
				key->state = RELEASE_DETECTED_STATE;
			} 
			else if (currentTime - key->pressStartTime > LONG_PRESS_THRESHOLD) {
				key->state = LONG_PRESS_TRIGGER_STATE;
			}
			break;

			case LONG_PRESS_TRIGGER_STATE:
			if (keyStatus == 0) { 
				if (key->onLongPress != NULL) {
					key->onLongPress(); 
				}
				key->state = LONG_PRESS_HOLD_STATE;
				key->lastLongAddTime = currentTime;
			} else {
				key->state = LONG_PRESS_STATE_END;
			}
			break;

			case LONG_PRESS_HOLD_STATE:
			if (keyStatus == 1) { 
				key->state = LONG_PRESS_STATE_END;
			} 
			else {
				if (currentTime - key->lastLongAddTime >= LONG_PRESS_ADD_INTERVAL) {
					if (key->onLongPress != NULL) {
						key->onLongPress(); 
					}
					key->lastLongAddTime = currentTime;
				}
			}
			break;

			case LONG_PRESS_STATE_END:
			if (keyStatus == 1) { 
				key->state = IDLE_STATE;
			}
			break;

			case RELEASE_DETECTED_STATE:
			if ((keyStatus == 0) && (currentTime - key->releaseTime < SHORT_CLICK_THRESHOLD)) {
				key->state = DOUBLE_PRESS_STATE;
			} 
			else if ((key->releaseTime - key->pressStartTime > PRESS_Time) && (currentTime - key->releaseTime > SHORT_CLICK_THRESHOLD)) {
				key->state = SHORT_PRESS_STATE;
			} 
			else if (currentTime - key->releaseTime > BUTTON_ERROR_Time) {
				key->state = IDLE_STATE;
			}
			break;

			case SHORT_PRESS_STATE:
			if (key->onShortPress != NULL) {
				key->onShortPress(); 
			}
			key->state = LONG_PRESS_STATE_END;
			break;

			case DOUBLE_PRESS_STATE:
			if (key->onDoublePress != NULL) {
				key->onDoublePress(); 
			}
			key->state = LONG_PRESS_STATE_END;
			break;
		}
	}
}


