#include "recordingpixels.h"

#include <QImage>
#include <array>
#include <cmath>
#include <cstring>
#include <emmintrin.h>

namespace
{
__m128i PackBytes(__m128i values)
{
	const auto words = _mm_packs_epi32(values, values);
	return _mm_packus_epi16(words, words);
}

// BT.709 limited range, with the coefficients scaled by 256.
__m128i Luma4(__m128i bgra)
{
	const auto br = _mm_and_si128(bgra, _mm_set1_epi32(0x00ff00ff));
	const auto g = _mm_and_si128(_mm_srli_epi32(bgra, 8), _mm_set1_epi32(0xff));
	auto y = _mm_add_epi32(_mm_madd_epi16(br, _mm_set1_epi32((47 << 16) | 16)),
							_mm_madd_epi16(g, _mm_set1_epi32(157)));
	y = _mm_add_epi32(_mm_srli_epi32(_mm_add_epi32(y, _mm_set1_epi32(128)), 8), _mm_set1_epi32(16));
	return PackBytes(y);
}

__m128i Chroma4(__m128i top, __m128i bottom)
{
	const auto zero = _mm_setzero_si128();
	const auto left = _mm_add_epi16(_mm_unpacklo_epi8(top, zero), _mm_unpacklo_epi8(bottom, zero));
	const auto right = _mm_add_epi16(_mm_unpackhi_epi8(top, zero), _mm_unpackhi_epi8(bottom, zero));
	const auto sums = _mm_unpacklo_epi64(_mm_add_epi16(left, _mm_srli_si128(left, 8)),
											_mm_add_epi16(right, _mm_srli_si128(right, 8)));
	auto u = _mm_madd_epi16(sums, _mm_setr_epi16(112, -86, -26, 0, 112, -86, -26, 0));
	auto v = _mm_madd_epi16(sums, _mm_setr_epi16(-10, -102, 112, 0, -10, -102, 112, 0));
	u = _mm_shuffle_epi32(_mm_add_epi32(u, _mm_srli_si128(u, 4)), _MM_SHUFFLE(2, 0, 2, 0));
	v = _mm_shuffle_epi32(_mm_add_epi32(v, _mm_srli_si128(v, 4)), _MM_SHUFFLE(2, 0, 2, 0));
	const auto uv = _mm_unpacklo_epi32(u, v);
	return PackBytes(_mm_add_epi32(_mm_srai_epi32(_mm_add_epi32(uv, _mm_set1_epi32(512)), 10), _mm_set1_epi32(128)));
}

void Store4(uchar* out, __m128i value)
{
	const int low = _mm_cvtsi128_si32(value);
	std::memcpy(out, &low, sizeof(low));
}
}

void PackRecordingNv12(const QImage& image, QByteArray& buffer)
{
	const int width = image.width();
	const int height = image.height();
	Q_ASSERT(image.format() == QImage::Format_RGB32 && width % 2 == 0 && height % 2 == 0);
	const int lumaSize = width * height;
	buffer.resize(lumaSize + lumaSize / 2);
	auto* const luma = reinterpret_cast<uchar*>(buffer.data());
	for (int row = 0; row < height; row += 2)
	{
		const auto* top = image.constScanLine(row);
		const auto* bottom = image.constScanLine(row + 1);
		auto* yTop = luma + row * width;
		auto* yBottom = yTop + width;
		auto* uv = luma + lumaSize + row / 2 * width;
		int x = 0;
		for (; x + 3 < width; x += 4)
		{
			const auto a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(top + 4 * x));
			const auto b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(bottom + 4 * x));
			Store4(yTop + x, Luma4(a));
			Store4(yBottom + x, Luma4(b));
			Store4(uv + x, Chroma4(a, b));
		}
		if (x < width)
		{
			const auto a = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(top + 4 * x));
			const auto b = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(bottom + 4 * x));
			const auto first = static_cast<quint16>(_mm_cvtsi128_si32(Luma4(a)));
			const auto second = static_cast<quint16>(_mm_cvtsi128_si32(Luma4(b)));
			const auto color = static_cast<quint16>(_mm_cvtsi128_si32(Chroma4(a, b)));
			std::memcpy(yTop + x, &first, sizeof(first));
			std::memcpy(yBottom + x, &second, sizeof(second));
			std::memcpy(uv + x, &color, sizeof(color));
		}
	}
}

namespace
{
constexpr double ReferenceWhite = 203.;

double Pq(double nits)
{
	const double p = std::pow(qBound(0., nits / 10000., 1.), .1593017578125);
	return std::pow((.8359375 + 18.8515625 * p) / (1. + 18.6875 * p), 78.84375);
}

double DecodePq(double value)
{
	const double p = std::pow(value, 1. / 78.84375);
	return 10000. * std::pow(qMax(0., p - .8359375) / qMax(1.e-6, 18.8515625 - 18.6875 * p), 1. / .1593017578125);
}
}
QImage ExpandSdrRecordingFrame(const QImage& input)
{
	static const auto linear = []
	{
		std::array<int, 256> values{};
		for (int i = 0; i < 256; ++i)
		{
			const double x = i / 255.;
			values[i] = qRound(4095. * (x <= .04045 ? x / 12.92 : std::pow((x + .055) / 1.055, 2.4)));
		}
		return values;
	}();
	static const auto encoder = []
	{
		std::array<quint16, 4096> values{};
		for (int i = 0; i < 4096; ++i) values[i] = quint16(qRound(Pq(i * ReferenceWhite / 4095.) * 65535.));
		return values;
	}();
	const QImage source = input.format() == QImage::Format_RGB32 ? input : input.convertToFormat(QImage::Format_RGB32);
	QImage result(source.size(), QImage::Format_RGBA64);
	for (int y = 0; y < source.height(); ++y)
	{
		const auto in = reinterpret_cast<const QRgb*>(source.constScanLine(y));
		auto out = reinterpret_cast<QRgba64*>(result.scanLine(y));
		for (int x = 0; x < source.width(); ++x)
		{
			const int r = linear[qRed(in[x])], g = linear[qGreen(in[x])], b = linear[qBlue(in[x])];
			out[x] = QRgba64::fromRgba64(encoder[(642 * r + 337 * g + 45 * b + 512) >> 10],
				encoder[(71 * r + 942 * g + 11 * b + 512) >> 10],
				encoder[(17 * r + 90 * g + 917 * b + 512) >> 10], 65535);
		}
	}
	return result;
}

QImage ToneMapRecordingPreview(const QImage& image)
{
	if (image.format() != QImage::Format_RGBA64) return image;
	static const auto decoder = []
	{
		std::array<float, 4096> values{};
		for (int i = 0; i < 4096; ++i) values[i] = float(DecodePq(i / 4095.) / ReferenceWhite);
		return values;
	}();
	static const auto srgb = []
	{
		std::array<int, 4096> values{};
		for (int i = 0; i < 4096; ++i)
		{
			const double x = i / 4095.;
			values[i] = qRound(255. * (x <= .0031308 ? 12.92 * x : 1.055 * std::pow(x, 1. / 2.4) - .055));
		}
		return values;
	}();
	QImage result(image.size(), QImage::Format_RGB32);
	for (int y = 0; y < image.height(); ++y)
	{
		const auto in = reinterpret_cast<const QRgba64*>(image.constScanLine(y));
		auto out = reinterpret_cast<QRgb*>(result.scanLine(y));
		for (int x = 0; x < image.width(); ++x)
		{
			const double r = decoder[in[x].red() >> 4], g = decoder[in[x].green() >> 4], b = decoder[in[x].blue() >> 4];
			const auto encode = [](double value) { return srgb[qRound(4095. * qBound(0., value, 1.))]; };
			const double maxValue = qMax(1., qMax(r, qMax(g, b)));
			out[x] = qRgb(encode((1.660491 * r - .587641 * g - .07285 * b) / maxValue),
				encode((-.12455 * r + 1.1329 * g - .008349 * b) / maxValue),
				encode((-.018151 * r - .100579 * g + 1.11873 * b) / maxValue));
		}
	}
	return result;
}
