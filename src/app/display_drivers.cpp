#include "board_config.h"

#if HAS_DISPLAY

// Arduino build system only compiles .cpp files in the sketch root directory.
// This translation unit centralizes the required driver implementation includes
// so manager code stays focused on logic.

#if DISPLAY_DRIVER == DISPLAY_DRIVER_IT8951
#include "drivers/it8951_lvgl_driver.cpp"
#else
#error "No display driver selected or unknown driver type"
#endif

#endif // HAS_DISPLAY
