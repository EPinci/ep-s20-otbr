#ifndef _LED_H_
#define _LED_H_

#define CONFIG_LED_PWR_RED GPIO_NUM_14    // Power
#define CONFIG_LED_PWR_GREEN GPIO_NUM_9   // Power
#define CONFIG_LED_PWR_YELLOW GPIO_NUM_20 // Power

#define CONFIG_LED_THREAD_RED GPIO_NUM_18    // Iot
#define CONFIG_LED_THREAD_GREEN GPIO_NUM_17  // Iot
#define CONFIG_LED_THREAD_YELLOW GPIO_NUM_19 // Iot

#define CONFIG_LED_NWK_RED GPIO_NUM_7     // NETWORK
#define CONFIG_LED_NWK_GREEN GPIO_NUM_6   // NETWORK
#define CONFIG_LED_NWK_YELLOW GPIO_NUM_15 // NETWORK

#define LED_ON 255
#define LED_OFF 0

int led_init();
void set_nwk_led_color(bool red, bool green, bool yellow);
void set_mesh_led_color(bool red, bool green, bool yellow);
void set_pwr_led_color(bool red, bool green, bool yellow);

#endif /* _LED_H_ */