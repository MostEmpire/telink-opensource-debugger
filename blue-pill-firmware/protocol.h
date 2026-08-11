/* Host <-> bridge command protocol, carried over the USB CDC byte stream.
 *
 *   request : 0x55 CMD LEN_LO LEN_HI payload[LEN]
 *   reply   : 0x55 STS LEN_LO LEN_HI payload[LEN]
 *
 * Deliberately identical in framing to the older UART bridge so the existing
 * Python gdbserver transport works unchanged.  The firmware stays a dumb, fast,
 * timing-accurate SWire pipe; all CPU/flash semantics live in the host layer.
 */
#ifndef PROTOCOL_H
#define PROTOCOL_H

#define BR_SYNC          0x55
#define FW_VERSION       0x0002          /* reported by PING / GET_CFG */
#define FW_IDENT         "TLSRSWS2"      /* 8 bytes, no NUL on the wire */

/* ------------------------------------------------------------ commands --- */
#define CMD_PING         0x01  /* -                        -> "TLSRSWS2" + ver16 */
#define CMD_SYNC         0x02  /* [div?]                   -> -                  */
#define CMD_SWS_WRITE    0x03  /* [a2 a1 a0][data...]      -> -                  */
#define CMD_SWS_READ     0x04  /* [a2 a1 a0][n_lo n_hi]    -> n bytes            */
#define CMD_RESET        0x05  /* [mode]                   -> -                  */
#define CMD_SET_SPEED    0x06  /* [div 0..7]               -> -                  */
#define CMD_PWR          0x07  /* [0=off 1=on]([rails])    -> -                  */
#define CMD_SET_CFG      0x08  /* [cell low0 low1 thr abytes sbits soff slack]   */
#define CMD_GET_CFG      0x09  /* -    -> cfg(8) ver16 gpioA16 gpioB16 rxlen16   */
#define CMD_ACTIVATE     0x0A  /* [n_lo n_hi]([a2 a1 a0][data])  -> sent16       */
#define CMD_GET_RAW      0x0B  /* [off_lo off_hi n_lo n_hi] -> raw MISO samples  */
#define CMD_SELFTEST     0x0C  /* -    -> [echo_ok][decoded bytes...]            */
#define CMD_PINTEST      0x0D  /* -    -> [ok][PA6 low][PA6 high][PA6 released]  */
#define CMD_FLASH_RD     0x0F  /* [a2 a1 a0][n16] -> n flash bytes         */
#define CMD_FLASH_WR     0x10  /* [a2 a1 a0][data...] page program + poll  */
#define CMD_ACT_READ     0x0E  /* [n16][a2 a1 a0][len] -> [used16][data...]      */

/* Target power rails, bitmask for CMD_PWR (default = both).
 *
 *   PB0    drives the target's +3.3V pad straight from the GPIO.  Limited to
 *          a few mA -- fine with the core stalled, browns out under radio load.
 *   PB12   gate of an external P-channel MOSFET high-side switch.  ACTIVE LOW
 *          (pulling the gate down turns the FET on); an external 10K to +3.3V
 *          plus the internal pull-up hold it off by default.
 *
 * Both are asserted together unless a mask says otherwise. */
#define PWR_RAIL_PB0     0x01
#define PWR_RAIL_MOSFET  0x02
#define PWR_RAIL_BOTH    0x03

/* CMD_RESET modes */
#define RESET_SOFT       0     /* SWire write 0x20 -> reg 0x006F                 */
#define RESET_PWR_PULSE  1     /* power off, pause, power on                     */
#define RESET_PWR_ACT    2     /* power-cycle then run the activation burst      */

/* -------------------------------------------------------------- status --- */
#define ST_OK            0x00
#define ST_BAD_CMD       0x01
#define ST_BAD_LEN       0x02
#define ST_SWS_TIMEOUT   0x03  /* not enough slave bits decoded                  */
#define ST_NOSYNC        0x04  /* frame echo did not decode as START 0x5A        */
#define ST_BUSY          0x05
#define ST_NO_POWER      0x06  /* target power is off                            */

/* --------------------------------------------------------------- sizes --- */
#define MAX_PAYLOAD      256   /* SWire data bytes per request                   */
#define MAX_CELL         12    /* upper bound on SPI bits per SWire cell         */
#define SPI_BUF_SZ       3584  /* worst case encode of a MAX_PAYLOAD frame       */
#define CMD_BUF_SZ       (MAX_PAYLOAD + 16)

#endif /* PROTOCOL_H */
