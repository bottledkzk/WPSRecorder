#include "recordingframestream.h"

namespace
{
int IntegerBytes(quint64 value)
{
	int bytes = 1;
	while (value > 0xff)
	{
		value >>= 8;
		++bytes;
	}
	return bytes;
}

void AppendBigEndian(QByteArray& stream, quint64 value, int bytes)
{
	for (int i = bytes - 1; i >= 0; --i)
		stream.append(static_cast<char>((value >> (8 * i)) & 0xff));
}

void AppendElementHeader(QByteArray& stream, quint32 tag, quint64 length)
{
	AppendBigEndian(stream, tag, IntegerBytes(tag));
	int bytes = 1;
	while (length >= (quint64{1} << (7 * bytes)) - 1)
		++bytes;
	AppendBigEndian(stream, length | (quint64{1} << (7 * bytes)), bytes);
}

void AppendElement(QByteArray& stream, quint32 tag, const QByteArray& value)
{
	AppendElementHeader(stream, tag, value.size());
	stream.append(value);
}

void AppendInteger(QByteArray& stream, quint32 tag, quint64 value)
{
	const int bytes = IntegerBytes(value);
	AppendElementHeader(stream, tag, bytes);
	AppendBigEndian(stream, value, bytes);
}
}

QByteArray RecordingVideoHeader(int width, int height, int fps, bool nv12, bool hdrPixels)
{
	QByteArray stream;
	QByteArray ebml;
	AppendInteger(ebml, 0x4286, 1); // EBMLVersion
	AppendInteger(ebml, 0x42F7, 1); // EBMLReadVersion
	AppendInteger(ebml, 0x42F2, 4); // EBMLMaxIDLength
	AppendInteger(ebml, 0x42F3, 8); // EBMLMaxSizeLength
	AppendElement(ebml, 0x4282, "matroska");
	AppendInteger(ebml, 0x4287, 4); // DocTypeVersion
	AppendInteger(ebml, 0x4285, 2); // DocTypeReadVersion
	AppendElement(stream, 0x1A45DFA3, ebml);
	stream.append(QByteArray::fromHex("1853806701ffffffffffffff")); // Segment of unknown length

	QByteArray info;
	AppendInteger(info, 0x2AD7B1, 1000); // TimestampScale, one microsecond in nanoseconds
	AppendElement(info, 0x4D80, "WPS Recorder"); // MuxingApp
	AppendElement(info, 0x5741, "WPS Recorder"); // WritingApp
	AppendElement(stream, 0x1549A966, info);

	QByteArray video;
	AppendInteger(video, 0xB0, width); // PixelWidth
	AppendInteger(video, 0xBA, height); // PixelHeight
	AppendElement(video, 0x2EB524, hdrPixels ? "RBA@" : nv12 ? "NV12" : "BGRA"); // UncompressedFourCC
	QByteArray track;
	AppendInteger(track, 0xD7, 1); // TrackNumber
	AppendInteger(track, 0x73C5, 1); // TrackUID
	AppendInteger(track, 0x83, 1); // Video
	AppendInteger(track, 0x9C, 0); // FlagLacing
	AppendElement(track, 0x86, "V_UNCOMPRESSED");
	AppendInteger(track, 0x23E383, (1000000000LL + fps / 2) / fps); // DefaultDuration in nanoseconds
	AppendElement(track, 0xE0, video);
	QByteArray tracks;
	AppendElement(tracks, 0xAE, track);
	AppendElement(stream, 0x1654AE6B, tracks);
	return stream;
}

QByteArray RecordingFrameHeader(qint64 slot, int fps, qint64 pixelBytes)
{
	QByteArray clusterTimestamp;
	AppendInteger(clusterTimestamp, 0xE7, (slot * 1000000 + fps / 2) / fps);
	QByteArray block;
	constexpr int BlockPrefixBytes = 4;
	AppendElementHeader(block, 0xA3, BlockPrefixBytes + pixelBytes);
	// Track 1, zero relative timestamp, keyframe. The raw pixels follow this header.
	block.append("\x81\x00\x00\x80", BlockPrefixBytes);
	QByteArray header;
	AppendElementHeader(header, 0x1F43B675, clusterTimestamp.size() + block.size() + pixelBytes);
	header.append(clusterTimestamp);
	header.append(block);
	return header;
}
