#include "uilanguage.h"
#include "speech.h"
#include "speechservices.h"
#include "editproject.h"
#include "recordingconfig.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <cmath>
namespace
{
QString ValidateSpeech(const QString& text, const QString& output, const KRRVoiceOptions& options)
{
	if (text.trimmed().isEmpty() || text.size() > 4000)
		return Ui("请输入 1–4000 字的旁白。");
	if (!std::isfinite(options.speed) || options.speed < .5 || options.speed > 2 || !std::isfinite(options.volume) ||
		options.volume < 0 || options.volume > 1.5)
		return Ui("配音速度或音量无效。");
	if (QFileInfo::exists(output))
		return Ui("目标文件已存在，请选择新文件名。");
	if (QFileInfo(output).suffix().toLower() != "wav")
		return Ui("独立配音请保存为 WAV。");
	return {};
}

QString NormalizeSpeech(const QString& raw, const QString& ready, const QString& output, const KRRVoiceOptions& options,
	const std::atomic<bool>* cancel)
{
	QString error;
	QString filter = QString("volume=%1,alimiter=limit=0.95:level=0").arg(options.volume, 0, 'f', 4);
	if (std::abs(options.speed - 1.0) > .001 &&
		(options.engine == "audio8" || (IsTtsService(options.engine) && SpeechService(options.engine).protocol == "dashscope")))
		filter.prepend(QString("atempo=%1,").arg(options.speed, 0, 'f', 4));
	if (!RunMediaProcess(FindFFmpeg(),
						 {"-v", "error", "-nostdin", "-protocol_whitelist", "file,pipe", "-i", raw, "-af", filter, "-ac", "1",
						  "-ar", "48000", "-c:a", "pcm_s16le", ready},
						 error, 120000, cancel, true))
		return error;
	if (!QFile::rename(ready, output))
		return Ui("无法保存配音文件。");
	return {};
}
}

QString SynthesizeSpeech(const QString &text, const QString &output, const KRRVoiceOptions &options,
						 const std::atomic<bool> *cancel)
{
	if (const auto error = ValidateSpeech(text, output, options); !error.isEmpty()) return error;
	if (options.engine != "audio8" && !IsTtsService(options.engine)) return Ui("未知配音引擎。");
	QTemporaryDir temp(QFileInfo(output).absolutePath() + "/.voice-XXXXXX");
	if (!temp.isValid()) return Ui("无法创建配音工作目录。");
	const auto raw = temp.filePath("raw.wav");
	const auto error = SynthesizeWithService(text, raw, options, cancel);
	if (!error.isEmpty()) return error;
	return NormalizeSpeech(raw, temp.filePath("speech.wav"), output, options, cancel);
}

QString SynthesizeAudio8Speech(const QVector<KRRVoiceLine>& lines, const KRRVoiceOptions& options,
	const std::atomic<bool>* cancel)
{
	if (lines.isEmpty()) return Ui("配音文本为空。");
	QTemporaryDir temp(QFileInfo(lines.front().output).absolutePath() + "/.voice-XXXXXX");
	if (!temp.isValid()) return Ui("无法创建配音工作目录。");
	QVector<KRRVoiceLine> raw;
	for (const auto& line : lines)
	{
		if (const auto error = ValidateSpeech(line.text, line.output, options); !error.isEmpty()) return error;
		raw.append({line.text, temp.filePath(QString("raw-%1.wav").arg(raw.size()))});
	}
	if (const auto error = SynthesizeAudio8Raw(raw, options.voice, cancel); !error.isEmpty()) return error;
	for (int i = 0; i < lines.size(); ++i)
	{
		const auto error = NormalizeSpeech(raw[i].output, temp.filePath("speech.wav"), lines[i].output, options, cancel);
		if (!error.isEmpty()) return error;
	}
	return {};
}

QString MixNarration(const QString &input, const QString &speech, const QString &output, double begin,
					 double originalVolume, const std::atomic<bool> *cancel)
{
	if (!std::isfinite(begin) || begin < 0 || !std::isfinite(originalVolume) || originalVolume < 0 ||
		originalVolume > 1)
		return Ui("旁白位置或原声音量无效。");
	if (QFileInfo::exists(output))
		return Ui("目标文件已存在，请选择新文件名。");
	auto ext = QFileInfo(output).suffix().toLower();
	if (!QStringList{"mp4", "mov", "mkv"}.contains(ext))
		return Ui("旁白视频支持 MP4/MOV/MKV。");
	KRREditProject project;
	QString error;
	if (!ProbeEditSource(input, project, error))
		return error;
	QProcess probe;
	HideConsoleProcess(probe);
	probe.start(FindFFprobe(), {"-v", "error", "-show_format", "-of", "json", speech});
	if (!probe.waitForFinished(15000) || probe.exitCode() != 0)
		return Ui("无法读取旁白音频。");
	double duration = QJsonDocument::fromJson(probe.readAllStandardOutput())
						  .object()["format"]
						  .toObject()["duration"]
						  .toString()
						  .toDouble();
	if (duration <= 0 || !std::isfinite(duration))
		return Ui("旁白音频时长无效。");
	if (begin + duration > project.duration / 1000. + .05)
		return Ui("旁白超过成片结尾，请缩短文字、加快语速或调整起点。");
	QTemporaryDir temp(QFileInfo(output).absolutePath() + "/.narration-XXXXXX");
	if (!temp.isValid())
		return Ui("无法创建旁白合成目录。");
	QString filter, original;
	if (!project.audio.isEmpty())
	{
		for (int i = 0; i < project.audio.size(); ++i)
			original += QString("[0:a:%1]").arg(i);
		filter = original + QString("amix=inputs=%1:normalize=0,volume=%2[original];")
								.arg(project.audio.size())
								.arg(originalVolume, 0, 'f', 4);
	}
	filter += QString("[1:a:0]adelay=%1:all=1,apad,atrim=duration=%2[voice];")
				  .arg(qRound64(begin * 1000))
				  .arg(project.duration / 1000., 0, 'f', 3);
	filter += project.audio.isEmpty() ? "[voice]anull[mix]"
									  : "[original][voice]amix=inputs=2:normalize=0,alimiter=limit=0.95:level=0[mix]";
	auto partial = temp.filePath("result." + ext);
	QStringList args{"-v",
					 "error",
					 "-nostdin",
					 "-i",
					 input,
					 "-i",
					 speech,
					 "-filter_complex",
					 filter,
					 "-map",
					 "0:v:0",
					 "-map",
					 "[mix]",
					 "-c:v",
					 "copy",
					 "-c:a",
					 "aac",
					 "-b:a",
					 "192k",
					 "-t",
					 QString::number(project.duration / 1000., 'f', 3)};
	if (ext != "mkv")
		args << "-movflags" << "+faststart";
	args << partial;
	if (!RunMediaProcess(FindFFmpeg(), args, error, 7200000, cancel))
		return error;
	if (!QFile::rename(partial, output))
		return Ui("无法保存带旁白视频。");
	return {};
}
