#ifndef FUSB302_I2C_DRV_H
#define FUSB302_I2C_DRV_H

/*
 * Interface of platform-dependent i2c driver
 */
typedef struct {
    // Initiate async read/write operations.
    // Signatures and method name will be clarified later.
    void (*write)();
    void (*read)();

    // required for `pt_wait_cond()` polling
    bool (*done)();

    // "get_last error()"
    int (*get_status)();

    // MUST be called by every async i2c function, when done.
    // Used to invoke pt scheduler and continue processing
    void (*on_complete)();
} fusb302_i2c_drv_t;

#endif // FUSB302_I2C_DRV_H