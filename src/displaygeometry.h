#ifndef WPSRECORDER_DISPLAYGEOMETRY_H
#define WPSRECORDER_DISPLAYGEOMETRY_H
#include <windows.h>
// Call before constructing Qt: every capture/selection coordinate is a native pixel.
inline void InitializeDisplayCoordinates()
{
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        SetProcessDPIAware();
}
#endif
