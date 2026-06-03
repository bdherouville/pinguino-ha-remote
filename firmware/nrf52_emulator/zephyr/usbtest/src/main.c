/* NCS blinky: prove the NCS toolchain + board + 0.9.2 bootloader run.
 * VCC on (P0.13 high), blink P0.15 (active-low user LED) at 2 Hz. No BLE. */
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
static const struct device *g0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));
int main(void){
    gpio_pin_configure(g0,13,GPIO_OUTPUT_HIGH);  /* VCC on */
    gpio_pin_configure(g0,15,GPIO_OUTPUT_HIGH);  /* LED off (active-low) */
    for(;;){ gpio_pin_set(g0,15,0); k_msleep(250); gpio_pin_set(g0,15,1); k_msleep(250); }
}
