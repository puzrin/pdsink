Google EC TCPMv2 port.

- https://chromium.googlesource.com/chromiumos/platform/ec/+/HEAD/docs/sitemap.md

Difference:

- Dropped non-SINK code.
  - Still left top USBC layer to detect connector insert. For boards with
    dual power. For example, when extra USB used for ESP32 debug.
    Can reconsider this later - if PD always enabled - need to restore from
    error state after timeout.
- PD task replaced with event loop.
- Timer simplified to use 1ms ticks getTime(). You should setup timer handle.
- Drivers:
  - Wrapped with protothreads, to keep logic of I2C calls simple.
  - Dropped unused functions.
  - I2C driver simplified, but still has locking feature, to share bus with
    multiple devices.
