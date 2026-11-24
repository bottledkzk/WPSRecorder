#include "recordingrecovery.h"
#include "editproject.h"
#include "recordingconfig.h"
#include "recordingmarkers.h"
#include "uilanguage.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryDir>

static bool IsWorkspace(const QFileInfo& directory)
{
	static const QRegularExpression name(
		"^\\.wpsrecorder-[a-f0-9]{8}(?:-[a-f0-9]{4}){3}-[a-f0-9]{12}$",
		QRegularExpression::CaseInsensitiveOption);
	return directory.isDir() && !directory.isSymLink() && !directory.isJunction() &&
		   name.match(directory.fileName()).hasMatch();
}

static QString Material(const QString& workspace, const QString& name)
{
	const QFileInfo file(QDir(workspace).filePath(name));
	return file.isFile() && !file.isSymLink() && file.size() > 0 ? file.absoluteFilePath() : QString();
}

static QJsonObject ReadManifest(const QString& workspace)
{
	QFile file(Material(workspace, "recovery.json"));
	if (!file.open(QIODevice::ReadOnly) || file.size() > 16384) return {};
	return QJsonDocument::fromJson(file.readAll()).object();
}

void WriteRecordingRecovery(const QString& workspace, const QString& output, const QString& stage, const QString& error)
{
	QSaveFile file(QDir(workspace).filePath("recovery.json"));
	if (!file.open(QIODevice::WriteOnly)) return;
	const auto bytes = QJsonDocument(QJsonObject{
		{"version", 1}, {"output_name", QFileInfo(output).fileName()},
		{"stage", stage}, {"error", error.left(3000)}
	}).toJson(QJsonDocument::Compact);
	if (file.write(bytes) == bytes.size()) file.commit();
}

QVector<KRRRecoveryItem> RecordingRecoveryItems(const QString& directory)
{
	QVector<KRRRecoveryItem> items;
	const QDir root(directory);
	for (const auto& info : root.entryInfoList({".wpsrecorder-*"}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Time))
	{
		if (!IsWorkspace(info)) continue;
		const auto workspace = info.absoluteFilePath();
		if (Material(workspace, "video.mkv").isEmpty()) continue;
		// A completed recording can retain its folder if cleanup was interrupted.
		if (!Material(workspace, "complete").isEmpty()) continue;
		QLockFile lock(QDir(workspace).filePath("recording.lock"));
		lock.setStaleLockTime(0);
		if (!lock.tryLock(0)) continue;
		const auto manifest = ReadManifest(workspace);
		KRRRecoveryItem item;
		item.workspace = workspace;
		item.name = manifest["output_name"].toString(info.fileName());
		item.stage = manifest["stage"].toString();
		if (item.stage == "recording") item.stage = "interrupted";
		item.error = manifest["error"].toString();
		if (!Material(workspace, "mic.f32").isEmpty()) item.audio << "mic";
		if (!Material(workspace, "system.f32").isEmpty()) item.audio << "system";
		for (int i = 0; i < MaxAdditionalAudioSources; ++i)
		{
			const auto name = QString("extra-%1").arg(i);
			if (!Material(workspace, name + ".f32").isEmpty()) item.audio << name;
		}
		if (!Material(workspace, "camera.mkv").isEmpty()) item.hasCamera = true;
		item.modified = info.lastModified();
		items.append(item);
	}
	return items;
}

QString RecoverRecording(const QString& workspace, const QString& output, const std::atomic<bool>* cancel)
{
	if (!IsWorkspace(QFileInfo(QDir::cleanPath(workspace))))
		return Ui("请选择有效的录制恢复目录。");
	QLockFile lock(QDir(workspace).filePath("recording.lock"));
	lock.setStaleLockTime(0);
	if (!lock.tryLock(0)) return Ui("该录制仍在写入或正在恢复。");
	const auto video = Material(workspace, "video.mkv");
	if (video.isEmpty()) return Ui("这次录制没有可恢复的视频画面。");
	if (QFileInfo::exists(output) || QFileInfo::exists(RecordingMarkersPath(output)) || output.contains(QChar(0)))
		return Ui("目标文件已存在，请选择新文件名。");
	const auto ext = QFileInfo(output).suffix().toLower();
	if (ext != "mkv" && ext != "mp4") return Ui("恢复素材请保存为 MP4 或 MKV。");
	const auto outDir = QFileInfo(output).absolutePath();
	QTemporaryDir temp(outDir + "/.wps-recovery-XXXXXX");
	if (!temp.isValid()) return Ui("无法创建恢复文件，请检查保存目录。");
	QStringList args{"-nostdin", "-hide_banner", "-v", "error", "-fflags", "+discardcorrupt", "-i", video};
	QStringList audio;
	QStringList tracks{"mic", "system"};
	for (int i = 0; i < MaxAdditionalAudioSources; ++i) tracks << QString("extra-%1").arg(i);
	for (const auto& name : tracks)
	{
		const auto source = Material(workspace, name + ".f32");
		if (source.isEmpty() || QFileInfo(source).size() < 8) continue;
		args << "-f" << "f32le" << "-ar" << "48000" << "-ac" << "2" << "-i" << source;
		audio << name;
	}
	args << "-map" << "0:v:0" << "-c:v" << "copy";
	for (int i = 0; i < audio.size(); ++i)
	{
		args << "-map" << QString("%1:a:0").arg(i + 1)
			 << QString("-filter:a:%1").arg(i) << "apad"
			 << QString("-metadata:s:a:%1").arg(i) << "title=" + audio[i]
			 << QString("-metadata:s:a:%1").arg(i) << "handler_name=" + audio[i];
	}
	if (!audio.isEmpty()) args << "-c:a" << "aac" << "-b:a" << "192k" << "-shortest";
	if (ext == "mp4") args << "-movflags" << "+faststart";
	const auto staged = temp.filePath("recovered." + ext);
	args << staged;
	QString error;
	if (!RunMediaProcess(FindFFmpeg(), args, error, 7200000, cancel)) return error;
	KRREditProject project;
	if (!ProbeEditSource(staged, project, error)) return error;
	if (!QFile::rename(staged, output)) return Ui("无法保存恢复件，请检查目标目录或文件占用。");
	auto markers = ReadRecordingMarkers(video);
	if (!markers.isEmpty() && !QFileInfo::exists(RecordingMarkersPath(output)))
		WriteRecordingMarkers(RecordingMarkersPath(output), markers, error);
	return {};
}
