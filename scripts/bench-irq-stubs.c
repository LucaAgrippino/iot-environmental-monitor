/* Bench builds only: Src/stm32l4xx_it.c calls the sensor DRDY handlers, and a bench image
 * that links no sensor driver has no definition for them (the IDE build links every driver). */
void magnetometer_drdy_irq_handler(void)
{
}

void imu_int1_irq_handler(void)
{
}
