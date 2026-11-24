#include "uilanguage.h"
#include "mediaeditor.h"
#include "recordingrecovery.h"
#include "recordingconfig.h"
#include "speechservices.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QUuid>
#include <QTemporaryDir>
#include <cmath>
#include <functional>
#include <windows.h>

KRRMediaEditor::KRRMediaEditor(QObject* parent) : QObject(parent)
{
	qRegisterMetaType<QVector<QString>>();
	qRegisterMetaType<KRRSubtitleReview>();
	qRegisterMetaType<KRRSequenceSmartCutDraft>();
}
KRRMediaEditor::~KRRMediaEditor() { cancel();if (m_thread.joinable()) m_thread.join(); }
bool KRRMediaEditor::isBusy() const { return m_busy.load(); }
bool KRRMediaEditor::launch(const QString& output, std::function<QString()> task)
{
	if (m_busy.exchange(true)) return false;
	if (m_thread.joinable()) m_thread.join();
	m_cancel.store(false);
	emit busyChanged();
	m_thread = std::thread([this, output, task]
	{
		QString error = task();
		m_busy.store(false);
		emit busyChanged();
		emit completed(output, error);
	});
	return true;
}
static bool ProbeMedia(const QString& input, double& duration, bool& audio, QString& error, const std::atomic<bool>& cancel)
{
	if (cancel.load())
	{
		error = Ui("任务已取消。");
		return false;
	}
	QProcess probe;
	HideConsoleProcess(probe);
	probe.start(FindFFprobe(), {"-v", "error", "-show_format", "-show_streams", "-of", "json", input});
	QElapsedTimer deadline;
	deadline.start();
	while (probe.state() != QProcess::NotRunning && !probe.waitForFinished(100))
	{
		if (cancel.load() || deadline.hasExpired(15000))
		{
			probe.kill();
			probe.waitForFinished(1500);
			error = cancel.load() ? Ui("任务已取消。") : "Cannot inspect media: " + input;
			return false;
		}
	}
	if (probe.error() == QProcess::FailedToStart || probe.exitStatus() != QProcess::NormalExit || probe.exitCode() != 0)
	{
		error = "Cannot inspect media: " + input;
		return false;
	}
	QJsonObject document = QJsonDocument::fromJson(probe.readAllStandardOutput()).object();
	duration = document["format"].toObject()["duration"].toString().toDouble();
	audio = false;
	bool video = false;
	for (const auto& stream : document["streams"].toArray())
	{
		QString type = stream.toObject()["codec_type"].toString();
		audio |= type == "audio";
		video |= type == "video";
	}
	if (!video || !std::isfinite(duration) || duration <= 0) { error = "Input needs a finite video duration"; return false; }
	return true;
}
static QString ExportClip(const QString& input, const QString& output, double begin, double end,
	const KRRExportOptions& options, const std::atomic<bool>& cancel)
{
	QString error;
	double duration = 0;
	bool audio = false;
	if (!ProbeMedia(input, duration, audio, error, cancel)) return error;
	if (end < 0) end = duration;
	if (!std::isfinite(begin) || !std::isfinite(end) || begin < 0 || end <= begin || end > duration + 0.1)
		return "Invalid interval; use 0 <= begin < end <= media duration";
	if (QFileInfo::exists(output)) return "Output already exists";
	QString extension = QFileInfo(output).suffix().toLower();
	if (!QStringList{"mp4", "mov", "mkv", "webm", "gif", "m4a", "mp3"}.contains(extension)) return "Unsupported export extension";
	bool audioOnly = extension == "m4a" || extension == "mp3";
	if (options.copyStreams && (begin != 0 || end < duration - .05 || options.denoise || !options.subtitles.isEmpty() || !QStringList{"mp4", "mov", "mkv"}.contains(extension))) return "Stream copy requires the complete video, MP4/MOV/MKV, and no audio cleanup or added subtitles";
	if (audioOnly && !audio) return "This video has no audio track to extract";
	if (extension == "gif" && end - begin > 60) return "GIF export is limited to 60 seconds; shorten the selected interval";
	if (!options.subtitles.isEmpty() && (extension == "gif" || audioOnly || !QFileInfo::exists(options.subtitles))) return "Subtitles require a video container and an existing SRT file";
	QString partial = QFileInfo(output).absolutePath() + "/.clip-" + QUuid::createUuid().toString(QUuid::WithoutBraces) + "." + extension;
	QStringList arguments{"-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-ss", QString::number(begin, 'f', 6), "-i", input};
	if (!options.subtitles.isEmpty()) arguments << "-ss" << QString::number(begin, 'f', 6) << "-i" << options.subtitles;
	arguments << "-t" << QString::number(end - begin, 'f', 6);
	if (options.copyStreams) arguments << "-map" << "0:v:0" << "-map" << "0:a?" << "-c" << "copy";
	else if (audioOnly) arguments << "-map" << "0:a:0" << "-vn" << "-c:a" << (extension == "mp3" ? "libmp3lame" : "aac") << "-b:a" << "192k";
	else if (extension == "gif") arguments << "-filter_complex" << "[0:v]fps=12,scale=960:-2:flags=lanczos,split[a][b];[a]palettegen[p];[b][p]paletteuse" << "-an" << "-loop" << "0";
	else
	{
		arguments << "-map" << "0:v:0" << "-map" << "0:a?";
		if (extension == "webm") arguments << "-c:v" << "libvpx-vp9" << "-deadline" << "realtime" << "-cpu-used" << "6" << "-b:v" << "4000k" << "-c:a" << "libopus";
		else
		{
			QString encoder = SelectVideoEncoder(options.encoder, error);
			if (encoder.isEmpty()) return error;
			arguments << VideoEncoderArguments(encoder, 6000, false) << "-c:a" << "aac";
		}
		if (!options.subtitles.isEmpty()) arguments << "-map" << "1:s:0" << "-c:s" << (extension == "webm" ? "webvtt" : (extension == "mkv" ? "srt" : "mov_text"));
	}
	if (options.denoise && audio && extension != "gif") arguments << "-af" << "highpass=f=80,lowpass=f=12000,afftdn=nf=-25,loudnorm=I=-16:TP=-1.5:LRA=11";
	if (extension == "mp4" || extension == "mov" || extension == "m4a") arguments << "-movflags" << "+faststart";
	arguments << partial;
	if (!RunMediaProcess(FindFFmpeg(), arguments, error, 7200000, &cancel)) { QFile::remove(partial); return error; }
	if (!QFile::rename(partial, output)) { QFile::remove(partial); return "Could not save exported clip"; }
	return {};
}
bool KRRMediaEditor::exportMedia(const QString& input, const QString& output, double begin, double end, const KRRExportOptions& options)
{
	return launch(output, [=] { return ExportClip(input, output, begin, end, options, m_cancel); });
}
bool KRRMediaEditor::renderTimeline(const KRREditProject& project, const QString& output, const QString& encoder,
									bool separateAudio, bool burnCaptions, const KRRTimelineExportOptions& options)
{
	return launch(output, [=] { return RenderEditProject(project, output, encoder, separateAudio, burnCaptions, &m_cancel, options); });
}
bool KRRMediaEditor::renderSequence(const KRREditSequence& sequence, const QString& output, bool separateAudio, bool preview)
{
	return launch(output, [=]
	{
		return RenderEditSequence(sequence, output, "auto", separateAudio, &m_cancel, [this](int done, int total)
		{
			emit progress(done == total ? Ui("正在封装音轨、字幕和章节…") :
				Ui("正在导出第 %1 / %2 段…").arg(done + 1).arg(total));
		}, preview);
	});
}
bool KRRMediaEditor::suggestSequenceCut(const KRREditSequence& sequence, const QString& goal, const KRRSequenceCutOptions& options)
{
	return launch({}, [=]
	{
		KRRSequenceSmartCutDraft draft;
		const auto error = SuggestSequenceSmartCut(sequence, goal, options, draft, &m_cancel,
			[this](const QString& message) { emit progress(message); });
		emit sequenceDrafted(draft);
		return error;
	});
}
bool KRRMediaEditor::recoverRecording(const QString& workspace, const QString& output)
{
	return launch(output, [=] { return RecoverRecording(workspace, output, &m_cancel); });
}
bool KRRMediaEditor::synthesize(const QString& text,const QString& output,const KRRVoiceOptions& options)
{
    return launch(output,[=]{emit progress(Ui("正在生成旁白，请稍候…"));return SynthesizeSpeech(text,output,options,&m_cancel);});
}
bool KRRMediaEditor::translateSubtitles(const QVector<QString>& source, const QString& target, const QString& glossary, int only)
{
	return launch({}, [=] {
		QVector<QString> result;
		const auto error = TranslateSubtitleLines(source, target, glossary, result, &m_cancel,
			[this](int done, int total) { emit progress(Ui("已翻译 %1 / %2 句…").arg(done).arg(total)); }, only);
		if (error.isEmpty()) emit translated(result);
		return error;
	});
}
bool KRRMediaEditor::reviewSubtitles(const KRREditProject& project, int first, int last, const QString& target, const QString& goal)
{
	return launch({}, [=]
	{
		KRRSubtitleReview review;
		const auto error = ReviewSubtitles(project, first, last, target, goal, review, &m_cancel, [this](int batch, int total)
		{
			emit progress(Ui("正在审核字幕（%1 / %2 批）…").arg(batch).arg(total));
		});
		if (error.isEmpty() && !m_cancel.load()) emit subtitlesReviewed(review);
		return error;
	});
}
bool KRRMediaEditor::buildDubbing(qint64 duration, const QVector<KRRDubLine>& lines, const QString& output)
{
	return launch(output, [=] { return MixDubbingTrack(duration, lines, output, &m_cancel); });
}
bool KRRMediaEditor::suggestSmartCut(const KRREditProject& project, const KRRSmartCutOptions& options)
{
	qRegisterMetaType<KRRSmartCutDraft>();
	return launch({}, [=] {
		KRRSmartCutDraft draft;
		const auto error = SuggestSmartCut(project, options, draft, &m_cancel,
			[this](int done, int total) {
				if (done == total) return;
				emit progress((done % 2 ? Ui("正在向视频模型分析第 %1 / %2 段…") : Ui("正在准备第 %1 / %2 段画面…"))
					.arg(done / 2 + 1).arg(total / 2));
			});
		if (error.isEmpty() && !m_cancel.load()) emit smartDrafted(draft);
		return error;
	});
}
bool KRRMediaEditor::narrateProject(const KRREditProject& project,const QString& speech,const QString& output,double begin,double originalVolume)
{
    return launch(output,[=]() -> QString {
        if(QFileInfo::exists(output))return Ui("目标文件已存在，请选择新文件名。");
        QTemporaryDir temp(QFileInfo(output).absolutePath()+"/.narration-edit-XXXXXX");if(!temp.isValid())return Ui("无法创建剪辑旁白目录。");
        emit progress(Ui("正在按当前剪辑计划生成画面与字幕…"));
        auto clip=temp.filePath("edited.mp4");auto error=RenderEditProject(project,clip,"auto",false,true,&m_cancel);
        if(!error.isEmpty())return error;if(m_cancel.load())return Ui("任务已取消。");
        emit progress(Ui("正在合成旁白与原声，视频流保持不重复编码…"));return MixNarration(clip,speech,output,begin,originalVolume,&m_cancel);
    });
}
bool KRRMediaEditor::transcribe(const QString& input, const QString& output, const QString& executable, const QString& model, const QString& language, int audioTrack)
{
	return launch(output, [=]() -> QString
	{
		if (QFileInfo::exists(output)) return "Subtitle output already exists";
		if (!QFileInfo::exists(executable) || !QFileInfo::exists(model)) return "Select whisper-cli.exe and an offline GGML model first";
		QTemporaryDir temporary(QFileInfo(output).absolutePath() + "/.subtitle-XXXXXX");
		if (!temporary.isValid()) return "Cannot create subtitle workspace";
		QString wav = temporary.filePath("speech.wav"), prefix = temporary.filePath("transcript"), error;
		if(audioTrack<0||audioTrack>15)return "Invalid subtitle audio track";
		if(!QStringList{"auto","zh","en","ja","fr","de","es","ko","it","pt","ru"}.contains(language))return "Unsupported subtitle language";
		emit progress(Ui("正在提取选定声音轨…"));
		if (!RunMediaProcess(FindFFmpeg(), {"-hide_banner", "-loglevel", "error", "-nostdin", "-i", input, "-map", QString("0:a:%1").arg(audioTrack), "-vn", "-ac", "1", "-ar", "16000", "-c:a", "pcm_s16le", wav}, error, 1800000, &m_cancel, true)) return error;
		emit progress(Ui("正在离线识别（")+language+Ui("），完成后可人工校对，可随时取消…"));
		if (!RunMediaProcess(executable, {"-m", model, "-f", wav, "-l", language, "-osrt", "-of", prefix, "-t", "8", "-ng","-ml","45","-sow"}, error, 7200000, &m_cancel, true)) return error;
		if(language=="zh")
		{
			QFile subtitle(prefix+".srt");if(!subtitle.open(QIODevice::ReadOnly))return Ui("无法读取识别结果。");
			QString text=QString::fromUtf8(subtitle.readAll());subtitle.close();
			int count=LCMapStringEx(L"zh-CN",LCMAP_SIMPLIFIED_CHINESE,reinterpret_cast<LPCWSTR>(text.utf16()),text.size(),nullptr,0,nullptr,nullptr,0);
			if(count>0){QString simplified(count,Qt::Uninitialized);LCMapStringEx(L"zh-CN",LCMAP_SIMPLIFIED_CHINESE,reinterpret_cast<LPCWSTR>(text.utf16()),text.size(),reinterpret_cast<LPWSTR>(simplified.data()),count,nullptr,nullptr,0);if(!subtitle.open(QIODevice::WriteOnly|QIODevice::Truncate)||subtitle.write(simplified.toUtf8())<0)return Ui("无法保存简体字幕。");subtitle.close();}
		}
		if (!QFileInfo::exists(prefix + ".srt") || !QFile::rename(prefix + ".srt", output)) return "Whisper did not produce a writable SRT output";
		return {};
	});
}
bool KRRMediaEditor::trim(const QString& input, const QString& output, double begin, double end)
{
	return launch(output, [=] { return ExportClip(input, output, begin, end, {}, m_cancel); });
}
bool KRRMediaEditor::split(const QString& input, const QString& directory, const QList<QPair<double, double>>& intervals)
{
	return launch(directory, [=]() -> QString
	{
		if (intervals.isEmpty()) return "Provide at least one interval";
		if (!QDir().mkpath(directory)) return "Cannot create split directory";
		QStringList created;
		for (int index = 0; index < intervals.size(); ++index)
		{
			QString output = QDir(directory).filePath(QString("part-%1.mp4").arg(index + 1, 3, 10, QLatin1Char('0')));
			QString error = ExportClip(input, output, intervals[index].first, intervals[index].second, {}, m_cancel);
			if (!error.isEmpty())
			{
				for (const auto& path : created) QFile::remove(path);
				return error;
			}
			created.append(output);
		}
		return {};
	});
}
bool KRRMediaEditor::merge(const QStringList& inputs, const QString& output, const KRRExportOptions& options)
{
	return launch(output, [=]() -> QString
	{
		if (inputs.size() < 2) return "Choose at least two videos";
		if (QFileInfo::exists(output)) return "Output already exists";
		if (QFileInfo(output).suffix().toLower() != "mp4") return "Editing exports use the .mp4 extension";
		QStringList arguments{"-hide_banner", "-loglevel", "error", "-nostdin", "-y"};
		QStringList filters;
		QString joined;
		for (int index = 0; index < inputs.size(); ++index)
		{
			double duration = 0;
			bool audio = false;
			QString error;
			if (!ProbeMedia(inputs[index], duration, audio, error, m_cancel)) return error;
			arguments << "-i" << inputs[index];
			filters << QString("[%1:v:0]scale=1280:720:force_original_aspect_ratio=decrease,pad=1280:720:(ow-iw)/2:(oh-ih)/2,setsar=1,fps=30,setpts=PTS-STARTPTS[v%1]").arg(index);
			QString source = audio ? QString("[%1:a:0]aresample=48000,aformat=channel_layouts=stereo,apad").arg(index)
				: "anullsrc=r=48000:cl=stereo";
			filters << source + QString(",atrim=duration=%1,asetpts=PTS-STARTPTS[a%2]").arg(duration, 0, 'f', 6).arg(index);
			joined += QString("[v%1][a%1]").arg(index);
		}
		filters << joined + QString("concat=n=%1:v=1:a=1[v][a]").arg(inputs.size());
		QString partial = QFileInfo(output).absolutePath() + "/.merge-" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".mp4";
		QString error;
		QString encoder = SelectVideoEncoder(options.encoder, error);
		if (encoder.isEmpty()) return error;
		arguments << "-filter_complex" << filters.join(';') << "-map" << "[v]" << "-map" << "[a]";
		arguments << VideoEncoderArguments(encoder, 6000, false) << "-c:a" << "aac" << "-movflags" << "+faststart" << partial;
		if (!RunMediaProcess(FindFFmpeg(), arguments, error, 1800000, &m_cancel)) { QFile::remove(partial); return error; }
		if (!QFile::rename(partial, output)) { QFile::remove(partial); return "Could not save merged video"; }
		return {};
	});
}
