#include "recordingmarkers.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <cmath>

QString RecordingMarkersPath(const QString& video)
{
	return video + ".markers.json";
}

QJsonArray ReadRecordingMarkers(const QString& video)
{
	const auto path = RecordingMarkersPath(video);
	if (QFileInfo(path).isSymLink()) return {};
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly) || file.size() > 524288) return {};
	const auto document = QJsonDocument::fromJson(file.readAll()).object();
	if (document["version"].toInt() != 1) return {};
	QJsonArray result;
	for (const auto& entry : document["markers"].toArray())
	{
		const auto marker = entry.toObject();
		const auto at = marker["at_ms"].toDouble(-1);
		if (at >= 0 && at <= 604800000 && std::floor(at) == at && marker["label"].isString())
			result.append(QJsonObject{{"at_ms", at}, {"label", marker["label"].toString().left(120)}});
		if (result.size() == 500) break;
	}
	return result;
}

bool WriteRecordingMarkers(const QString& path, const QJsonArray& markers, QString& error)
{
	QSaveFile file(path);
	const auto content = QJsonDocument(QJsonObject{{"version", 1}, {"markers", markers}}).toJson(QJsonDocument::Compact);
	if (file.open(QIODevice::WriteOnly) && file.write(content) == content.size() && file.commit()) return true;
	error = "Could not save recording markers";
	return false;
}
