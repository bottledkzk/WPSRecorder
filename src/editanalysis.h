#ifndef WPSRECORDER_EDITANALYSIS_H
#define WPSRECORDER_EDITANALYSIS_H
#include "editproject.h"
#include <QImage>
#include <QObject>
#include <atomic>
#include <thread>
class KRREditAnalysis : public QObject
{
	Q_OBJECT
  public:
	explicit KRREditAnalysis(QObject *parent = nullptr) : QObject(parent)
	{
	}
	~KRREditAnalysis() override;
	void start(const QString &input, const QString &cache);
	void stop();
	quint64 generation() const
	{
		return m_generation;
	}
  signals:
	void sourceReady(quint64 token, const KRREditProject &project);
	void thumbnail(quint64 token, int index, const QImage &image);
	void waveform(quint64 token, int track, const QVector<float> &envelope);
	void audioReady(quint64 token, int track, const QString &proxy);
	void progress(quint64 token, const QString &message);
	void failed(quint64 token, const QString &error);

  private:
	std::thread m_thread;
	std::atomic<bool> m_cancel{false};
	quint64 m_generation = 0;
};
Q_DECLARE_METATYPE(KRREditProject)
#endif
