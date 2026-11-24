#ifndef WPSRECORDER_RECORDINGMARKERS_H
#define WPSRECORDER_RECORDINGMARKERS_H
#include <QJsonArray>
#include <QString>

QString RecordingMarkersPath(const QString& video);
QJsonArray ReadRecordingMarkers(const QString& video);
bool WriteRecordingMarkers(const QString& path, const QJsonArray& markers, QString& error);

#endif
