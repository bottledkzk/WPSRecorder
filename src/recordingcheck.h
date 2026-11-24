#ifndef WPSRECORDER_RECORDINGCHECK_H
#define WPSRECORDER_RECORDINGCHECK_H

#include "recordingconfig.h"
#include <QImage>
#include <QJsonObject>
#include <memory>

class KRRLiveCapturePreview
{
public:
	struct Frame
	{
		QImage image;
		QString error;
		QJsonObject capture;
		qint64 sequence = 0;
		QImage camera;
		QString cameraError;
	};
	// An empty bound keeps the original pixels for on-screen lecture effects.
	KRRLiveCapturePreview(const KRRRecordingConfig& config, int intervalMs, QSize maximum = QSize(2560, 1440),
		bool compose = false);
	~KRRLiveCapturePreview();
	// Updates composition without replacing the primary capture session.
	void updateComposition(const KRRRecordingConfig& config);
	Frame latest();
private:
	struct Impl;
	std::unique_ptr<Impl> d;
};

QImage CaptureRecordingPreview(const KRRRecordingConfig& config, QJsonObject& captureInfo, QString& error);
void ApplyRecordingPreviewEffects(QImage& image, const KRRRecordingConfig& config);

#endif
