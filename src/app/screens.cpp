// ============================================================================
// Screen/Session Compilation Unit
// ============================================================================
// Arduino only compiles .cpp files in the sketch root.
// This unit pulls in optional screen/session implementations from subfolders.

#include "board_config.h"

#if HAS_DISPLAY

#if HAS_IMAGE_API
#include "screens/direct_image_screen.cpp"
#endif

#endif // HAS_DISPLAY

// ============================================================================
// Touch Driver Implementations
// ============================================================================
// Touch driver implementations are compiled via src/app/touch_drivers.cpp.
