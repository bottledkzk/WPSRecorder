#ifndef WPSRECORDER_PREVIEWCHANNEL_H
#define WPSRECORDER_PREVIEWCHANNEL_H
#include <QImage>
#include <memory>
struct KRRPreviewFrame
{
	QImage frame, camera;
	qint64 sequence = 0;
};
// One pending frame, one published frame. Preview work never queues behind recording.
class KRRPreviewPublisher
{
  public:
	KRRPreviewPublisher();
	~KRRPreviewPublisher();
	void reset();
	void clear();
	void submit(const QImage &frame, const QImage &camera, bool mirror, qint64 sequence);
	KRRPreviewFrame snapshot();

  private:
	struct Impl;
	std::unique_ptr<Impl> d;
};
class KRRPreviewReader
{
  public:
	KRRPreviewReader();
	~KRRPreviewReader();
	bool read(KRRPreviewFrame &frame);
	void reset();

  private:
	struct Impl;
	std::unique_ptr<Impl> d;
};
#endif
