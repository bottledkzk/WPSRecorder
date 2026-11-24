#ifndef WPSRECORDER_RECORDINGCHECKDIALOG_H
#define WPSRECORDER_RECORDINGCHECKDIALOG_H

#include "recordingconfig.h"
#include <QDialog>
#include <QElapsedTimer>
#include <QImage>
#include <QTimer>
#include <array>
#include <memory>

class KRRAudioCapture;
class KRRCameraCapture;
class KRRLiveCapturePreview;
class QLabel;
class QProgressBar;
class QPushButton;
class QProcess;

class KRRRecordingCheckDialog : public QDialog
{
	Q_OBJECT
public:
	KRRRecordingCheckDialog(const KRRRecordingConfig& config, const QString& source,
							const QJsonObject& lastStatus, QWidget* parent);
	~KRRRecordingCheckDialog() override;

signals:
	void trialRequested();

protected:
	void done(int result) override;
	void resizeEvent(QResizeEvent* event) override;

private:
	void requestPreview();
	void updateLivePreview();
	void displayPreview();
	void startCamera();
	void updateCamera();
	void updateTrial();
	void updateMeters();
	void updateReport();
	void startMeters();

	KRRRecordingConfig m_config;
	QJsonObject m_lastStatus;
	QJsonObject m_checks;
	QImage m_image;
	QImage m_cameraImage;
	QString m_sourceError;
	QLabel* m_preview = nullptr;
	QLabel* m_cameraPreview = nullptr;
	QLabel* m_cameraStatus = nullptr;
	QLabel* m_report = nullptr;
	QPushButton* m_refresh = nullptr;
	QPushButton* m_trial = nullptr;
	QProcess* m_probe = nullptr;
	QTimer m_timer;
	QTimer m_probeTimeout;
	QTimer m_cameraTimer;
	QTimer m_frameTimer;
	std::unique_ptr<KRRLiveCapturePreview> m_livePreview;
	qint64 m_frameSequence = 0;
	QElapsedTimer m_cameraActivity;
	std::unique_ptr<KRRCameraCapture> m_camera;
	quint64 m_cameraSequence = 0;
	bool m_cameraFailed = false;
	std::array<std::unique_ptr<KRRAudioCapture>, 2> m_audio;
	std::array<QProgressBar*, 2> m_levels{};
	std::array<QLabel*, 2> m_info{};
	std::array<float, 2> m_smoothed{};
};

#endif
