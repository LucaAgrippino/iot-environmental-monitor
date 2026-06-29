#ifndef STATUS_H
#define STATUS_H

/**
 * @brief Common status codes for Gateway drivers and middleware.
 *
 * Modules with their own error namespace (e.g. I2cDriver → i2c_err_t) retain
 * their own type.  CpuDriver uses status_t because its conditions (timeout,
 * hardware absent) are generic infrastructure concerns.
 */
typedef enum
{
    STATUS_OK              = 0, /**< Operation completed successfully. */
    STATUS_ERR_TIMEOUT     = 1, /**< Operation exceeded the guard window. */
    STATUS_ERR_HW          = 2, /**< Hardware responded unexpectedly. */
    STATUS_ERR_NULL_PTR    = 3, /**< A required pointer argument was NULL. */
    STATUS_ERR_NO_RESOURCE = 4, /**< Static pool is exhausted. */
    STATUS_ERR_INVALID_ARG = 5, /**< Argument value is out of the allowed range. */
} status_t;

#endif /* STATUS_H */
