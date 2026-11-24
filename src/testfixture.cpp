// testfixture.cpp | 3226535826@qq.com | 2026-10-31 | Moving visual and stereo-tone QA surface.
#include <QApplication>
#include <QAudioOutput>
#include <QElapsedTimer>
#include <QFile>
#include <QIODevice>
#include <QPainter>
#include <QTimer>
#include <QWidget>
#include <cmath>
#include <cstring>

class KRRToneSource : public QIODevice
{
public:
	explicit KRRToneSource(QObject* parent) : QIODevice(parent) { open(QIODevice::ReadOnly); }
	qint64 readData(char* data, qint64 bytes) override
	{
		qint64 frames = bytes / 4;
		for (qint64 index = 0; index < frames; ++index)
		{
			qint16 left = static_cast<qint16>(1800 * std::sin(m_sample * 6.28318530718 * 440 / 48000));
			qint16 right = static_cast<qint16>(1800 * std::sin(m_sample * 6.28318530718 * 880 / 48000));
			std::memcpy(data + index * 4, &left, 2); std::memcpy(data + index * 4 + 2, &right, 2);
			++m_sample;
		}
		return frames * 4;
	}
	qint64 writeData(const char*, qint64) override { return -1; }
	qint64 bytesAvailable() const override { return 48000 * 4 + QIODevice::bytesAvailable(); }
private:
	qint64 m_sample = 0;
};
class KRRTestSurface : public QWidget
{
public:
	KRRTestSurface()
	{
		setWindowTitle("WPS Recorder QA Surface"); resize(960, 600); move(160, 120);
		m_clock.start();
		connect(&m_timer, &QTimer::timeout, this, [this] { update(); }); m_timer.start(16);
		if (QCoreApplication::arguments().contains("--motion"))
		{
			connect(&m_motionTimer, &QTimer::timeout, this, [this]
			{
				++m_motionStep;
				setGeometry(m_motionStep % 2 ? QRect(280, 210, 780, 500) : QRect(160, 120, 960, 600));
			});
			m_motionTimer.start(1500);
		}
		QAudioFormat format; format.setCodec("audio/pcm"); format.setSampleRate(48000);
		format.setChannelCount(2); format.setSampleSize(16); format.setSampleType(QAudioFormat::SignedInt);
		format.setByteOrder(QAudioFormat::LittleEndian);
		if (QCoreApplication::arguments().contains("--tone"))
		{
			auto output = new QAudioOutput(format, this);
			output->start(new KRRToneSource(this));
		}
	}
protected:
	void paintEvent(QPaintEvent*) override
	{
		QPainter painter(this); painter.fillRect(rect(), QColor("#11243c"));
		painter.setPen(QColor("#cce5ff")); painter.setFont(QFont("Segoe UI", 24));
		painter.drawText(40, 70, "WPS Recorder  |  Native Capture Verification");
		painter.setFont(QFont("Consolas", 26));
		painter.drawText(40, 130, QString("Elapsed %1 s").arg(m_clock.elapsed() / 1000.0, 0, 'f', 3));
		painter.setPen(QPen(QColor("#344c6c"), 1));
		for (int index = 0; index < width(); index += 40) painter.drawLine(index, 160, index, height());
		for (int index = 160; index < height(); index += 40) painter.drawLine(0, index, width(), index);
		double time = m_clock.elapsed() / 1000.0;
		painter.setPen(Qt::NoPen); painter.setBrush(QColor("#31d6ad"));
		painter.drawEllipse(QPointF(width() / 2.0 + std::sin(time) * width() * 0.3,
			height() * 0.62 + std::cos(time * 1.3) * 90), 45, 45);
		painter.setBrush(QColor("#ffb84d"));
		painter.drawRect(static_cast<int>(time * 95) % width(), height() - 70, 80, 25);
	}
private:
	QElapsedTimer m_clock;
	QTimer m_timer;
	QTimer m_motionTimer;
	int m_motionStep = 0;
};
int main(int argc, char* argv[])
{
	QApplication::setAttribute(Qt::AA_DisableHighDpiScaling);
	QApplication app(argc, argv);
	KRRTestSurface surface;
	surface.show();
	QStringList arguments = app.arguments();
	int handleOption = arguments.indexOf("--handle-file");
	if (handleOption >= 0 && handleOption + 1 < arguments.size())
	{
		QFile handle(arguments[handleOption + 1]);
		if (handle.open(QIODevice::WriteOnly)) handle.write(QByteArray::number(static_cast<qulonglong>(surface.winId())));
	}
	return app.exec();
}
