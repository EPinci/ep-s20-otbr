#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "led.h"
#include "driver/gpio.h"

int led_nwk_init()
{
    esp_err_t ret;
    gpio_config_t io_conf = {0};

    memset(&io_conf, 0, sizeof(gpio_config_t));
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask =
        (1ULL << CONFIG_LED_NWK_RED) | (1ULL << CONFIG_LED_NWK_GREEN) | (1ULL << CONFIG_LED_NWK_YELLOW);
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 0;

    ret = gpio_config(&io_conf);
    if (ret)
        return -1;

    gpio_set_level(CONFIG_LED_NWK_RED, 1);
    gpio_set_level(CONFIG_LED_NWK_GREEN, 1);
    gpio_set_level(CONFIG_LED_NWK_YELLOW, 1);

    return 0;
}

int led_thread_init()
{
    esp_err_t ret;
    gpio_config_t io_conf = {0};

    memset(&io_conf, 0, sizeof(gpio_config_t));
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask =
        (1ULL << CONFIG_LED_THREAD_RED) | (1ULL << CONFIG_LED_THREAD_GREEN) | (1ULL << CONFIG_LED_THREAD_YELLOW);
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 0;

    ret = gpio_config(&io_conf);
    if (ret)
        return -1;

    gpio_set_level(CONFIG_LED_THREAD_RED, 1);
    gpio_set_level(CONFIG_LED_THREAD_GREEN, 1);
    gpio_set_level(CONFIG_LED_THREAD_YELLOW, 1);

    return 0;
}

int led_pwr_init()
{
    esp_err_t ret;
    gpio_config_t io_conf = {0};

    memset(&io_conf, 0, sizeof(gpio_config_t));
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask =
        (1ULL << CONFIG_LED_PWR_RED) | (1ULL << CONFIG_LED_PWR_GREEN) | (1ULL << CONFIG_LED_PWR_YELLOW);
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 0;

    ret = gpio_config(&io_conf);
    if (ret)
        return -1;

    gpio_set_level(CONFIG_LED_PWR_RED, 1);
    gpio_set_level(CONFIG_LED_PWR_GREEN, 0);
    gpio_set_level(CONFIG_LED_PWR_YELLOW, 1);

    return 0;
}

int led_init()
{
    esp_err_t ret;
    ret = led_pwr_init();
    if (ret)
        return -1;

    ret = led_nwk_init();
    if (ret)
        return -1;

    ret = led_thread_init();
    if (ret)
        return -1;

    return 0;
}

void set_nwk_led_color(bool red, bool green, bool yellow)
{
    gpio_set_level(CONFIG_LED_NWK_RED, !red);
    gpio_set_level(CONFIG_LED_NWK_GREEN, !green);
    gpio_set_level(CONFIG_LED_NWK_YELLOW, !yellow);
}

void set_mesh_led_color(bool red, bool green, bool yellow)
{
    gpio_set_level(CONFIG_LED_THREAD_RED, !red);
    gpio_set_level(CONFIG_LED_THREAD_GREEN, !green);
    gpio_set_level(CONFIG_LED_THREAD_YELLOW, !yellow);
}

void set_pwr_led_color(bool red, bool green, bool yellow)
{
    gpio_set_level(CONFIG_LED_PWR_RED, !red);
    gpio_set_level(CONFIG_LED_PWR_GREEN, !green);
    gpio_set_level(CONFIG_LED_PWR_YELLOW, !yellow);
}
