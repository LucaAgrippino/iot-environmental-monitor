/* Spike only: stm32l4xx_it.c calls the sensor DRDY handlers; this image links no sensor driver. */
void magnetometer_drdy_irq_handler(void)
{
}

void imu_int1_irq_handler(void)
{
}
