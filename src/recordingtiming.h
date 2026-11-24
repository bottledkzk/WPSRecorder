#ifndef WPSRECORDER_RECORDINGTIMING_H
#define WPSRECORDER_RECORDINGTIMING_H

#include <QElapsedTimer>
#include <QJsonObject>
#include <algorithm>
#include <array>
#include <vector>

// Optional diagnostics. Store the samples in memory and publish only at finalization.
class KRRRecordingTimings
{
public:
	enum Stage { Acquire, ColorSubmit, Readback, DesktopPixels, ImageCopies, Compose,
		Pacing, ConvertVideo, PipeBuffer, PipeWait, RepeatWait, StageCount };

	void add(Stage stage, qint64 nanoseconds)
	{
		auto& series = m_stages[stage];
		++series.count;
		series.total += nanoseconds;
		series.max = std::max(series.max, nanoseconds);
		if (nanoseconds >= 16000000) ++series.over16ms;
		if (series.samples.size() < 120000) series.samples.push_back(nanoseconds);
	}

	QJsonObject summary() const
	{
		static constexpr const char* names[StageCount] = {"dxgi_acquire", "gpu_color_submit", "dxgi_readback",
			"crop_resize", "capture_copies", "capture_compose", "pacing_wait", "encoder_pixel_conversion",
			"pipe_buffer", "pipe_wait", "repeat_pipe_wait"};
		QJsonObject result;
		for (int stage = 0; stage < StageCount; ++stage)
		{
			const auto& s = m_stages[stage];
			if (!s.count) continue;
			auto samples = s.samples;
			std::sort(samples.begin(), samples.end());
			const auto percentile = [&samples](std::size_t pct)
			{
				const auto i = (samples.size() * pct + 99) / 100 - 1;
				return samples[i] / 1000000.0;
			};
			result[names[stage]] = QJsonObject{{"count", s.count}, {"percentile_samples", static_cast<int>(samples.size())},
				{"mean_ms", s.total / (1000000.0 * s.count)}, {"p50_ms", percentile(50)}, {"p95_ms", percentile(95)},
				{"p99_ms", percentile(99)}, {"max_ms", s.max / 1000000.0}, {"over_16ms", s.over16ms}};
		}
		return result;
	}

private:
	struct Series
	{
		qint64 count = 0, total = 0, max = 0, over16ms = 0;
		std::vector<qint64> samples;
	};
	std::array<Series, StageCount> m_stages;
};

class KRRStageTimer
{
public:
	KRRStageTimer(KRRRecordingTimings* timings, KRRRecordingTimings::Stage stage) : m_timings(timings), m_stage(stage)
	{
		if (m_timings) m_clock.start();
	}
	~KRRStageTimer() { finish(); }
	KRRStageTimer(const KRRStageTimer&) = delete;
	KRRStageTimer& operator=(const KRRStageTimer&) = delete;

	void finish()
	{
		if (!m_timings) return;
		m_timings->add(m_stage, m_clock.nsecsElapsed());
		m_timings = nullptr;
	}

private:
	KRRRecordingTimings* m_timings;
	KRRRecordingTimings::Stage m_stage;
	QElapsedTimer m_clock;
};

#endif
