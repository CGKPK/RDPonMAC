#ifndef RDPInputBridge_h
#define RDPInputBridge_h

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// RDP keyboard flags
#define RDP_KBD_FLAGS_EXTENDED  0x0100
#define RDP_KBD_FLAGS_DOWN      0x4000
#define RDP_KBD_FLAGS_RELEASE   0x8000

// RDP mouse flags
#define RDP_PTR_FLAGS_WHEEL     0x0200
#define RDP_PTR_FLAGS_WHEEL_NEGATIVE 0x0100
#define RDP_PTR_FLAGS_MOVE      0x0800
#define RDP_PTR_FLAGS_DOWN      0x8000
#define RDP_PTR_FLAGS_BUTTON1   0x1000
#define RDP_PTR_FLAGS_BUTTON2   0x2000
#define RDP_PTR_FLAGS_BUTTON3   0x4000

// Scancode to macOS virtual keycode mapping
// Returns -1 if no mapping exists
int16_t rdp_scancode_to_mac_keycode(uint8_t scancode, uint16_t flags);

#ifdef __cplusplus
}
#endif

#endif
