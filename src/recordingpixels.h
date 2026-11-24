#ifndef WPSRECORDER_RECORDINGPIXELS_H
#define WPSRECORDER_RECORDINGPIXELS_H

class QByteArray;
class QImage;

// The recorder supplies an even-sized RGB32 image (BGRA bytes on Windows).
void PackRecordingNv12(const QImage& image, QByteArray& buffer);

// Full-range BT.2020/PQ RGBA64 working frames. SDR reference white is 203 nits.
QImage ExpandSdrRecordingFrame(const QImage& image);
QImage ToneMapRecordingPreview(const QImage& image);

#endif
