#ifndef WPSRECORDER_PLAYERWINDOW_H
#define WPSRECORDER_PLAYERWINDOW_H
#include <QDialog>
#include <QJsonArray>
#include <QPointer>
#include <QTimer>
class QMediaPlayer;
class QStackedWidget;
class QVBoxLayout;
class QHBoxLayout;
class QComboBox;
class QScreen;
class QListWidget;
class QPushButton;
class KRRPlayerWindow : public QDialog
{
	Q_OBJECT
  public:
	KRRPlayerWindow(QMediaPlayer *player, QWidget *inlineVideo, QWidget *parent);
	void enter(bool fullscreen = false);
	bool fillsWorkArea() const;
	void setRecordings(const QJsonArray& markers, const QStringList& recent, const QString& active);

  signals:
	void recordingRequested(const QString& path);

  protected:
	void keyPressEvent(QKeyEvent* event) override;
	void resizeEvent(QResizeEvent* event) override;
	bool eventFilter(QObject* object, QEvent* event) override;

  private:
	QMediaPlayer *m_player;
	QWidget *m_inline;
	QStackedWidget *m_home = nullptr;
	QVBoxLayout *m_stage;
	QVBoxLayout *m_layout;
	QHBoxLayout *m_content;
	int m_homeIndex = 0;
	QComboBox *m_mode;
	QListWidget *m_markers = nullptr, *m_recent = nullptr;
	QPushButton* m_exit = nullptr;
	QPushButton *m_chaptersToggle = nullptr, *m_recentToggle = nullptr;
	QWidget *m_header = nullptr, *m_controls = nullptr;
	QWidget *m_chaptersPanel = nullptr, *m_recentPanel = nullptr;
	QPointer<QScreen> m_screen;
	QTimer m_hideControls;
	bool m_immersive = false, m_chromeVisible = true;
	void fit();
	void exitMode();
	void setImmersive(bool immersive);
	void setChromeVisible(bool visible);
	void placeChrome();
};
#endif
