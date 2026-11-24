#include "uilanguage.h"
#include "editanalysis.h"
#include "recordingconfig.h"
#include <QDir>
#include <QElapsedTimer>
#include <QProcess>
#include <cmath>
#include <cstring>
KRREditAnalysis::~KRREditAnalysis()
{
	stop();
}
void KRREditAnalysis::stop()
{
	++m_generation;
	m_cancel.store(true);
	if (m_thread.joinable())
		m_thread.join();
}
void KRREditAnalysis::start(const QString &input, const QString &cache)
{
	stop();
	m_cancel.store(false);
	auto token = m_generation;
	m_thread = std::thread(
		[this, input, cache, token]
		{
			auto run = [this](const QStringList &args, int timeout = 120000) -> bool
			{
				QProcess p;
				HideConsoleProcess(p, true);
				p.start(FindFFmpeg(), args);
				if (!p.waitForStarted(5000))
					return false;
				QElapsedTimer timer;
				timer.start();
				while (!p.waitForFinished(100))
				{
					if (m_cancel.load() || timer.elapsed() > timeout)
					{
						p.kill();
						p.waitForFinished(2000);
						return false;
					}
				}
				return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
			};
			KRREditProject project;
			QString error;
			if (!ProbeEditSource(input, project, error))
			{
				if (!m_cancel.load())
					emit failed(token, error);
				return;
			}
			if (m_cancel.load())
				return;
			emit sourceReady(token, project);
			for (int i = 0; i < 12 && !m_cancel.load(); ++i)
			{
				QString path = QDir(cache).filePath(QString("thumb-%1.png").arg(i));
				if (run({"-v", "error", "-nostdin", "-y", "-ss",
						 QString::number(qMin(project.duration - 1, project.duration * i / 12) / 1000.0, 'f', 3), "-i",
						 input, "-frames:v", "1", "-vf",
						 "scale=160:90:force_original_aspect_ratio=decrease,pad=160:90:(ow-iw)/2:(oh-ih)/2", path},
						15000))
					emit thumbnail(token, i, QImage(path));
			}
			for (int track = 0; track < project.audio.size() && !m_cancel.load(); ++track)
			{
				emit progress(token,
							  Ui("正在准备 %1 的声音监看与波形，可继续剪辑画面…").arg(project.audio[track].title));
				QString proxy = QDir(cache).filePath(QString("audio-%1.m4a").arg(track));
				if (run({"-v", "error", "-nostdin", "-y", "-i", input, "-map", QString("0:a:%1").arg(track), "-vn",
						 "-c:a", "aac", "-b:a", "128k", "-ar", "48000", proxy},
						1800000))
					emit audioReady(token, track, proxy);
				if (m_cancel.load())
					return;
				QProcess p;
				HideConsoleProcess(p, true);
				p.start(FindFFmpeg(), {"-v", "error", "-nostdin", "-i", input, "-map", QString("0:a:%1").arg(track),
									   "-vn", "-ac", "1", "-ar", "8000", "-f", "f32le", "pipe:1"});
				if (!p.waitForStarted(5000))
					continue;
				QVector<float> envelope;
				QByteArray bytes;
				double sum = 0;
				int count = 0;
				const int bucket = qMax(1, static_cast<int>(std::ceil(project.duration * 8.0 / 2000)));
				auto consume = [&]
				{
					bytes += p.readAllStandardOutput();
					int available = bytes.size() / 4 * 4;
					for (int j = 0; j < available; j += 4)
					{
						float v;
						std::memcpy(&v, bytes.constData() + j, 4);
						if (std::isfinite(v))
							sum += v * v;
						if (++count == bucket)
						{
							envelope.append(static_cast<float>(std::sqrt(sum / count)));
							sum = 0;
							count = 0;
						}
					}
					bytes.remove(0, available);
				};
				QElapsedTimer timer;
				timer.start();
				while (p.state() != QProcess::NotRunning)
				{
					p.waitForReadyRead(100);
					consume();
					if (m_cancel.load() || timer.elapsed() > 1800000)
					{
						p.kill();
						p.waitForFinished(2000);
						return;
					}
				}
				consume();
				if (count)
					envelope.append(static_cast<float>(std::sqrt(sum / count)));
				if (p.exitCode() == 0)
					emit waveform(token, track, envelope);
			}
			if (!m_cancel.load())
				emit progress(token, Ui("素材已就绪。拖动时间轴定位/选区，灰色区间在预览与导出时跳过。"));
		});
}
