#ifndef WPSRECORDER_RECORDINGFRAMESTREAM_H
#define WPSRECORDER_RECORDINGFRAMESTREAM_H

#include <QByteArray>

// Wrap raw pixels for FFmpeg's stdin with their recording-clock timestamps.
QByteArray RecordingVideoHeader(int width, int height, int fps, bool nv12, bool hdrPixels = false);
QByteArray RecordingFrameHeader(qint64 slot, int fps, qint64 pixelBytes);

#endif
