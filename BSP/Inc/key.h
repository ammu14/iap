#ifndef __KEY_H__
#define __KEY_H__

#include "main.h"
void key_handle(void);

// 按键状态枚举（仅负责检测动作）
typedef enum {
	IDLE_STATE,                // 空闲
	PRESS_DETECTED_STATE,      // 检测到按下
	RELEASE_DETECTED_STATE,    // 检测到释放
	SHORT_PRESS_STATE,         // 单击动作
	LONG_PRESS_TRIGGER_STATE,  // 长按首次触发
	LONG_PRESS_HOLD_STATE,     // 长按保持
	DOUBLE_PRESS_STATE,        // 双击动作
	LONG_PRESS_STATE_END       // 动作结束
} ButtonState;

// 按键动作枚举（对应回调函数类型）
typedef enum {
	KEY_ACTION_SHORT,    // 单击
	KEY_ACTION_LONG,     // 长按（首次+持续）
	KEY_ACTION_DOUBLE    // 双击
} KeyAction;

// 阈值宏定义（可根据手感调整）
#define SHORT_CLICK_THRESHOLD    500     // 双击间隔（500ms）
#define LONG_PRESS_THRESHOLD     2000   // 长按触发（2000ms）
#define PRESS_Time               20     // 单击有效时间（20ms）
#define BUTTON_ERROR_Time        5000   // 超时重置（5000ms）
#define LONG_PRESS_ADD_INTERVAL  500    // 长按累加间隔（500ms）

// ========== 2. 按键核心结构体（框架核心） ==========
typedef struct {
	GPIO_TypeDef* gpioPort;          // 按键GPIO端口
	uint16_t      gpioPin;           // 按键GPIO引脚
	ButtonState   state;             // 按键当前状态
	uint16_t    pressStartTime;    // 按下起始时间
	uint16_t    releaseTime;       // 释放时间
	uint16_t    lastLongAddTime;   // 长按累加计时
	// 按键功能回调：每个动作绑定一个函数指针（核心扩展点）
	void (*onShortPress)(void);      // 单击回调
	void (*onLongPress)(void);       // 长按回调（首次+持续）
	void (*onDoublePress)(void);     // 双击回调
	const char*   keyName;           // 按键名称（用于调试/打印）
} KeyInfo;

// 按键0回调
void Key0_ShortPress_Callback(void);
void Key0_LongPress_Callback(void);
void Key0_DoublePress_Callback(void);

// 按键1回调
void Key1_ShortPress_Callback(void);
void Key1_LongPress_Callback(void);
void Key1_DoublePress_Callback(void);

// 按键2回调
void Key2_ShortPress_Callback(void);
void Key2_LongPress_Callback(void);
void Key2_DoublePress_Callback(void);


extern KeyInfo keys[];
extern const uint32_t key_count;
#define KEY_COUNT key_count
#endif
