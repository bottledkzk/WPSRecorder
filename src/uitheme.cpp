#include "uilanguage.h"
#include "uitheme.h"
#include "recordingassistanttools.h"
#include <windows.h>
#include <QApplication>
#include <QAbstractButton>
#include <QIconEngine>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QStyleFactory>
#include <QToolButton>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QHeaderView>
#include <QTableWidget>

namespace RecorderUi
{
int Px(int value)
{
	double scale=GetDpiForSystem()/96.0;
	if(auto screen=QGuiApplication::primaryScreen())
	{
		const auto available=screen->availableGeometry().size();
		scale=qMin(scale,qMin(available.width()/1050.0,available.height()/800.0));
	}
	return qMax(1,qRound(value*qMax(.75,scale)));
}
void FitTableColumn(QTableWidget* table, int column, const QString& sample)
{
	auto header = table->horizontalHeader();
	const int heading = header->fontMetrics().horizontalAdvance(table->horizontalHeaderItem(column)->text());
	const int cell = table->fontMetrics().horizontalAdvance(sample);
	header->setSectionResizeMode(column, QHeaderView::Fixed);
	header->resizeSection(column, qMax(heading, cell) + Px(20));
}

// Vector paths are painted at the requested device size; no raster icon scaling.
class GlyphEngine : public QIconEngine
{
public:

	explicit GlyphEngine(QString name, QColor enabled = {}, QColor disabled = {})
		: m_name(std::move(name)), m_enabled(enabled), m_disabled(disabled) {}

	QIconEngine* clone() const override { return new GlyphEngine(m_name, m_enabled, m_disabled); }
	QString iconName() const override { return m_name; }

	void paint(QPainter* p, const QRect& rect, QIcon::Mode mode, QIcon::State) override
	{
		p->save(); p->setRenderHint(QPainter::Antialiasing); p->translate(rect.topLeft()); p->scale(rect.width() / 24.0, rect.height() / 24.0);
		QColor color(mode == QIcon::Disabled ? "#7c8795" : m_name == "floating-play" ? "#ff8593" :
			m_name == "floating-stop" ? "#fff3f5" : m_name == "record" || m_name == "done" ? "#ffffff" : "#cad5e4");
		const auto overrideColor = mode == QIcon::Disabled ? m_disabled : m_enabled;
		if (overrideColor.isValid()) color = overrideColor;
		p->setPen(QPen(color, 1.65, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)); p->setBrush(Qt::NoBrush);
		if (m_name == "record") { p->setPen(Qt::NoPen); p->setBrush(color); p->drawEllipse(QRectF(6.5, 6.5, 11, 11)); }
		else if (m_name == "pause") { p->drawLine(QPointF(8, 6), QPointF(8, 18)); p->drawLine(QPointF(16, 6), QPointF(16, 18)); }
		else if (m_name == "play" || m_name == "floating-play") { QPainterPath path; path.moveTo(8, 5); path.lineTo(19, 12); path.lineTo(8, 19); path.closeSubpath(); p->drawPath(path); }
		else if (m_name == "stop" || m_name == "floating-stop") { p->drawRoundedRect(QRectF(6, 6, 12, 12), 2, 2); }
		else if (m_name == "mic") { p->drawRoundedRect(QRectF(9, 3, 6, 12), 3, 3); p->drawArc(QRectF(6, 6, 12, 12), 180 * 16, 180 * 16); p->drawLine(QPointF(12, 18), QPointF(12, 22)); p->drawLine(QPointF(8, 22), QPointF(16, 22)); }
		else if (m_name == "camera") { p->drawRoundedRect(QRectF(3, 6, 13, 12), 3, 3); QPainterPath a; a.moveTo(16, 10); a.lineTo(21, 7); a.lineTo(21, 17); a.lineTo(16, 14); p->drawPath(a); }
		else if (m_name == "screenshot")
		{
			p->drawPolygon(QPolygonF({{3, 7}, {7, 7}, {9, 4}, {15, 4}, {17, 7}, {21, 7}, {21, 20}, {3, 20}}));
			p->drawEllipse(QPointF(12, 13), 3, 3);
		}
		else if (m_name == "grip")
		{
			p->setPen(Qt::NoPen);
			p->setBrush(color);
			for (int x : {9, 15})
				for (int y : {5, 12, 19}) p->drawEllipse(QPointF(x, y), 1.5, 1.5);
		}
		else if (m_name == "stroke-width")
		{
			p->drawLine(4, 6, 20, 6);
			p->drawLine(4, 12, 20, 12);
			p->setPen(QPen(color, 3, Qt::SolidLine, Qt::RoundCap));
			p->drawLine(4, 18, 20, 18);
		}
		else if (m_name == "click-effect")
		{
			p->drawEllipse(QPointF(12, 12), 3.5, 3.5);
			p->setPen(QPen(color, 1.65, Qt::DashLine, Qt::RoundCap));
			p->drawArc(QRectF(3, 3, 18, 18), 90 * 16, 285 * 16);
		}
		else if (m_name == "settings") { p->drawLine(4, 7, 20, 7); p->drawLine(4, 17, 20, 17); p->setBrush(QColor("#ffffff")); p->drawEllipse(QPointF(9, 7), 2.5, 2.5); p->drawEllipse(QPointF(15, 17), 2.5, 2.5); }
		else if (m_name == "edit" || m_name == "scissors") { p->drawLine(9, 10, 20, 4); p->drawLine(9, 14, 20, 20); p->drawEllipse(QRectF(3, 6, 6, 6)); p->drawEllipse(QRectF(3, 12, 6, 6)); }
		else if (m_name == "previous" || m_name == "next")
		{
			if (m_name == "next") { p->translate(24, 0); p->scale(-1, 1); }
			p->drawLine(5, 6, 5, 18);
			p->drawPolygon(QPolygonF({{8, 12}, {18, 5}, {18, 19}}));
		}
		else if (m_name == "crop") { p->drawLine(7, 3, 7, 17); p->drawLine(7, 17, 21, 17); p->drawLine(3, 7, 17, 7); p->drawLine(17, 7, 17, 21); }
		else if (m_name == "link")
		{
			p->translate(12, 12); p->rotate(-42);
			p->drawRoundedRect(QRectF(-10, -3.5, 8, 7), 3, 3);
			p->drawRoundedRect(QRectF(2, -3.5, 8, 7), 3, 3);
			p->drawLine(-5, 0, 5, 0);
		}
		else if (m_name == "clock") { p->drawEllipse(QRectF(4, 4, 16, 16)); p->drawLine(12, 7, 12, 12); p->drawLine(12, 12, 16, 14); }
		else if (m_name == "pen") { QPainterPath a; a.moveTo(5, 15); a.lineTo(16, 4); a.lineTo(20, 8); a.lineTo(9, 19); a.lineTo(4, 20); a.closeSubpath(); p->drawPath(a); p->drawLine(13, 7, 17, 11); }
		else if (m_name == "export") { p->drawLine(12, 4, 12, 15); p->drawLine(8, 8, 12, 4); p->drawLine(12, 4, 16, 8); QPainterPath a; a.moveTo(5, 13); a.lineTo(5, 20); a.lineTo(19, 20); a.lineTo(19, 13); p->drawPath(a); }
		else if (m_name == "folder") { QPainterPath a; a.moveTo(3, 7); a.lineTo(3, 19); a.lineTo(21, 19); a.lineTo(21, 8); a.lineTo(12, 8); a.lineTo(10, 5); a.lineTo(3, 5); a.closeSubpath(); p->drawPath(a); }
		else if (m_name == "assistant")
		{
			p->drawRoundedRect(QRectF(5, 8, 14, 11), 2, 2);
			p->drawLine(12, 4, 12, 8);
			p->drawLine(9, 13, 9, 14);
			p->drawLine(15, 13, 15, 14);
		}
		else if (m_name == "screen") { p->drawRoundedRect(QRectF(3, 4, 18, 13), 2, 2); p->drawLine(12, 17, 12, 21); p->drawLine(8, 21, 16, 21); }
		else if (m_name == "minimal") { p->drawRoundedRect(QRectF(3, 9, 18, 7), 3, 3); p->drawEllipse(QPointF(8, 12.5), 1, 1); p->drawLine(12, 12.5, 17, 12.5); }
		else if (m_name == "image") { p->drawRoundedRect(QRectF(3, 4, 18, 16), 2, 2); p->drawEllipse(QPointF(9, 9), 1.5, 1.5); QPainterPath a; a.moveTo(4, 18); a.lineTo(10, 12); a.lineTo(13, 15); a.lineTo(16, 11); a.lineTo(21, 16); p->drawPath(a); }
		else if (m_name == "check" || m_name == "done") { p->drawLine(5, 12, 10, 17); p->drawLine(10, 17, 19, 7); }
		else if (m_name == "more") { p->setBrush(color); for (int x : {5, 12, 19}) p->drawEllipse(QPointF(x, 12), 1, 1); }
		else if (m_name == "refresh") { p->drawArc(QRectF(5, 5, 14, 14), 40 * 16, 290 * 16); p->drawLine(19, 4, 19, 9); p->drawLine(14, 9, 19, 9); }
		else if (m_name == "full") { for (int sx : {-1, 1}) for (int sy : {-1, 1}) { int x = 12 + sx * 8, y = 12 + sy * 8; p->drawLine(x, y, x - sx * 5, y); p->drawLine(x, y, x, y - sy * 5); } }
		else if (m_name == "arrow") { p->drawLine(4, 20, 19, 5); p->drawLine(11, 5, 19, 5); p->drawLine(19, 5, 19, 13); }
		else if (m_name == "pointer") { QPolygonF a; a << QPointF(5, 3) << QPointF(19, 12) << QPointF(12, 13) << QPointF(9, 20); p->drawPolygon(a); }
		else if (m_name == "rectangle" || m_name == "whiteboard") { p->drawRect(QRect(4, 5, 16, 14)); }
		else if (m_name == "highlight")
		{
			p->drawPolygon(QPolygonF({{5, 17}, {6, 12}, {15, 3}, {21, 9}, {12, 18}, {7, 19}, {4, 21}, {3, 20}}));
			p->drawLine(6, 12, 12, 18);
			p->drawLine(6, 21, 14, 21);
		}
		else if (m_name == "eraser")
		{
			p->drawPolygon(QPolygonF({{6, 18}, {3, 15}, {3, 13}, {12, 4}, {14, 4}, {21, 11}, {21, 13}, {16, 18}}));
			p->drawLine(7, 10, 15, 18);
			p->drawLine(16, 18, 21, 18);
		}
		else if (m_name == "text") { p->drawLine(5, 6, 19, 6); p->drawLine(12, 6, 12, 19); p->drawLine(8, 19, 16, 19); }
		else if (m_name == "magnifier") { p->drawEllipse(QRect(4, 4, 11, 11)); p->drawLine(14, 14, 21, 21); }
		else if (m_name == "spotlight") { p->drawEllipse(QRect(4, 4, 16, 16)); p->drawEllipse(QRect(9, 9, 6, 6)); }
		else if (m_name == "pixel" || m_name == "solid")
		{
			p->drawRect(QRect(4, 4, 16, 16));
			if (m_name == "solid") p->fillRect(QRect(5, 5, 14, 14), color);
			else { p->drawLine(4, 12, 20, 12); p->drawLine(12, 4, 12, 20); }
		}
		else if (m_name == "color")
		{
			for (const auto& point : {QPoint(7, 7), QPoint(17, 7), QPoint(7, 17), QPoint(17, 17)})
				p->drawEllipse(QRect(point - QPoint(2, 2), QSize(4, 4)));
		}
		else if (m_name == "trash") { p->drawLine(4, 6, 20, 6); p->drawLine(9, 3, 15, 3); p->drawRect(QRect(7, 7, 10, 13)); }
		else if (m_name == "flag") { p->drawLine(6, 3, 6, 21); p->drawRect(QRect(6, 4, 13, 9)); }
		else if (m_name == "close") { p->drawLine(6, 6, 18, 18); p->drawLine(18, 6, 6, 18); }
		else if (m_name == "collapse") { p->drawLine(6, 9, 12, 15); p->drawLine(12, 15, 18, 9); }
		else if (m_name == "chevron-right") { p->drawLine(9, 6, 15, 12); p->drawLine(15, 12, 9, 18); }
		else if (m_name == "undo" || m_name == "redo")
		{
			if (m_name == "redo") { p->translate(24, 0); p->scale(-1, 1); }
			p->drawLine(4, 10, 9, 5); p->drawLine(4, 10, 9, 15); p->drawLine(4, 10, 15, 10);
			p->drawArc(QRect(11, 10, 8, 9), -90 * 16, 180 * 16);
		}
		else { p->drawRoundedRect(QRectF(5, 4, 14, 16), 2, 2); p->drawLine(8, 9, 16, 9); p->drawLine(8, 14, 14, 14); }
		p->restore();
	}

	QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override
	{
		QPixmap result(size); result.fill(Qt::transparent); QPainter painter(&result); paint(&painter, result.rect(), mode, state); return result;
	}
private:
	QString m_name;
	QColor m_enabled, m_disabled;
};
QIcon Icon(const QString& name) { return QIcon(new GlyphEngine(name)); }

QIcon ToolbarTheme::icon(const QString& name) const
{
	if (!isLight()) return Icon(name);
	const QColor foreground = name == "floating-play" ? record : name == "floating-stop" ? QColor("#fffaf5") : text;
	return QIcon(new GlyphEngine(name, foreground, muted));
}

static constexpr auto ToolbarThemeKey = "studioToolbarTheme";

static ToolbarTheme ParseToolbarTheme(const QJsonObject& data)
{
	if (!data["light"].toBool()) return {};
	const auto color = [&data](const char* key) { return QColor(data[key].toString()); };
	const auto head = color("head"), accent = color("accent");
	ToolbarTheme theme{{}, color("border"), color("text"), color("field"), color("divider"), color("hover"),
		color("selected"), color("selectedBorder"), color("muted"), color("record")};
	for (const auto& value : {head, accent, theme.border, theme.text, theme.field, theme.divider, theme.hover,
		theme.selected, theme.selectedBorder, theme.muted, theme.record})
		if (!value.isValid()) return {};
	// Match the 84/16 blend used for the Studio toolbar surfaces.
	theme.background = QColor(qRound(head.red() * .84 + accent.red() * .16),
		qRound(head.green() * .84 + accent.green() * .16), qRound(head.blue() * .84 + accent.blue() * .16));
	return theme;
}

ToolbarTheme ReadToolbarTheme()
{
	QSettings settings(RecorderUiSettingsPath(), QSettings::IniFormat);
	settings.setIniCodec("UTF-8");
	return ParseToolbarTheme(QJsonDocument::fromJson(settings.value(ToolbarThemeKey).toByteArray()).object());
}

bool WriteToolbarTheme(const QJsonObject& data, ToolbarTheme& theme)
{
	theme = ParseToolbarTheme(data);
	if (!data["light"].isBool() || (data["light"].toBool() && !theme.isLight())) return false;
	QSettings settings(RecorderUiSettingsPath(), QSettings::IniFormat);
	settings.setIniCodec("UTF-8");
	const auto serialized = QJsonDocument(data).toJson(QJsonDocument::Compact);
	if (settings.value(ToolbarThemeKey).toByteArray() != serialized)
	{
		settings.setValue(ToolbarThemeKey, serialized);
		settings.sync();
	}
	return settings.status() == QSettings::NoError;
}

void Apply(QApplication& app)
{
	app.setStyle(QStyleFactory::create("Fusion"));
	QPalette palette;
	palette.setColor(QPalette::Window, QColor("#202328"));
	palette.setColor(QPalette::WindowText, QColor("#e2e5ea"));
	palette.setColor(QPalette::Base, QColor("#191c21"));
	palette.setColor(QPalette::AlternateBase, QColor("#242831"));
	palette.setColor(QPalette::Text, QColor("#e2e5ea"));
	palette.setColor(QPalette::Button, QColor("#343943"));
	palette.setColor(QPalette::ButtonText, QColor("#e5e8ee"));
	palette.setColor(QPalette::Highlight, QColor("#3769a9"));
	palette.setColor(QPalette::HighlightedText, Qt::white);
	palette.setColor(QPalette::PlaceholderText, QColor("#8d96a4"));
	palette.setColor(QPalette::Disabled, QPalette::Text, QColor("#858b95"));
	app.setPalette(palette);
	QString sheet = QString::fromUtf8(R"qss(
QWidget { color:#e0e4eb; selection-background-color:#355c8c; selection-color:#ffffff; }
QMainWindow,QDialog { background:#202328; }
QLabel { background:transparent; }
QLabel[muted="true"] { color:#a4aebb; font-size:9pt; }
QLabel[heading="true"] { color:#ecf0f6; font-size:10pt; font-weight:600; }
QLabel[section="true"] { color:#bac3cf; font-size:9pt; font-weight:600; }
QFrame[panel="true"],QFrame#toolbarCard,QFrame#previewCard,QFrame#recordingCard {
 background:#282c34; border:1px solid #3a404a; border-radius:3px;
}
QFrame#studioHeader { background:#15191f; border-bottom:1px solid #333a45; }
QLabel#studioTitle { color:#f4f6fa; font-size:11pt; font-weight:700; }
QLabel#studioBadge { background:#162638; border:1px solid #345881; border-radius:3px; color:#9bc8fb; font-size:8pt; font-weight:700; padding:0 5px; }
QFrame[studioDock="true"] { background:#22262d; border:1px solid #393f48; border-radius:3px; }
QWidget#studioDockHeader { background:#272c35; border-bottom:1px solid #3a404a; }
QLabel[dockTitle="true"] { font-size:8.5pt; font-weight:600; color:#dce4ef; }
QFrame[studioDock="true"] QListWidget { background:#22262d; border:0; }
QFrame[studioDock="true"] QScrollArea { background:#22262d; border:0; }
QPushButton[role="windowControl"] { background:transparent; border:0; padding:1px; }
QPushButton[role="windowControl"]:hover { background:#343c47; }
QPushButton#studioClose:hover { background:#ad3544; }
QLabel#stateLabel { color:#aebed1; font-weight:600; }
QLabel#stateLabel[recording="true"] { color:#ff858d; }
QLabel#timeLabel { font-family:'Bahnschrift'; font-size:19pt; font-weight:600; color:#eef1f5; }
QLabel#recordingLight { color:#60d0a6; font-size:9pt; }
QLabel#studioMetrics { font-family:'Consolas','Microsoft YaHei UI'; font-size:8.5pt; color:#c2ccd8; }
QPushButton,QToolButton {
 min-height:17px; padding:2px 7px; border:1px solid #464d59; border-radius:3px;
 background:#343943; color:#e4e9f0; font-size:9pt;
}
QPushButton:hover,QToolButton:hover { background:#424a57; border-color:#586674; }
QPushButton:pressed,QToolButton:pressed { background:#2c303a; }
QPushButton:focus,QToolButton:focus { border-color:#76a7de; }
QPushButton:disabled,QToolButton:disabled { background:#282c33; border-color:#363b44; color:#88919e; }
QPushButton[role="primary"] { background:#356aa5; border-color:#427db8; color:white; font-weight:600; }
QPushButton[role="primary"]:hover { background:#3d7dbf; }
QPushButton[role="primary"]:disabled { background:#323c49; border-color:#3a4758; color:#a0acbb; }
QPushButton[role="startRecording"] { background:#171b21; border-color:#4a515e; color:#f0f3f7; font-weight:700; padding:6px 8px; }
QPushButton[role="startRecording"]:hover { background:#303946; }
QPushButton[role="startRecording"]:disabled { background:#20242b; color:#858d98; }
QPushButton[role="record"] { background:#a93947; border-color:#ca5860; color:white; font-weight:700; }
QPushButton[role="record"]:hover { background:#b84a57; }
QPushButton[role="record"]:disabled { background:#3c3339; border-color:#4b3d45; color:#b4979e; }
QPushButton[role="quiet"],QToolButton[role="quiet"] { background:transparent; border-color:transparent; }
QPushButton[role="quiet"]:hover,QToolButton[role="quiet"]:hover { background:#343c48; }
QPushButton:checked,QToolButton:checked { background:#304a68; border-color:#5180a9; color:#eff6ff; }
QPushButton[role="accent"] { color:#ffc6c9; }
QPushButton[role="workspace"] { background:transparent; border-color:transparent; padding:5px 10px; }
QPushButton[role="workspace"]:hover { background:#303742; }
QPushButton[role="workspace"]:checked { background:#354c66; border-color:#52789d; color:#f7fbff; }
QPushButton::menu-indicator { subcontrol-position:right center; right:4px; width:7px; }
QLineEdit,QComboBox,QSpinBox,QDoubleSpinBox,QDateTimeEdit,QKeySequenceEdit,QPlainTextEdit,QTextEdit {
 background:#1b1e24; color:#e0e7f0; min-height:17px; padding:2px 5px;
 border:1px solid #424953; border-radius:3px; font-size:9pt;
}
QLineEdit:focus,QComboBox:focus,QSpinBox:focus,QDoubleSpinBox:focus,QPlainTextEdit:focus { border-color:#6594c2; }
QLineEdit:disabled,QComboBox:disabled,QSpinBox:disabled,QDoubleSpinBox:disabled { background:#24272e; color:#89919c; }
QComboBox { padding-right:20px; }
QComboBox::drop-down { width:19px; border:0; }
QComboBox::down-arrow { image:url(:/ui/chevron-down.svg); width:12px; height:12px; }
QSpinBox::up-button,QDoubleSpinBox::up-button { subcontrol-position:top right; width:18px; border-left:1px solid #424953; }
QSpinBox::down-button,QDoubleSpinBox::down-button { subcontrol-position:bottom right; width:18px; border-left:1px solid #424953; }
QSpinBox::up-arrow,QDoubleSpinBox::up-arrow { image:url(:/ui/chevron-up.svg); width:10px; height:10px; }
QSpinBox::down-arrow,QDoubleSpinBox::down-arrow { image:url(:/ui/chevron-down.svg); width:10px; height:10px; }
QComboBox QAbstractItemView { background:#292e37; border:1px solid #454d58; padding:3px; outline:0; }
QCheckBox { spacing:6px; padding:2px 0; font-size:9pt; }
QCheckBox#studioDenoise:checked { color:#53d8b2; }
QCheckBox::indicator { width:14px; height:14px; }
QCheckBox::indicator:unchecked { background:#1c2027; border:1px solid #5a6576; border-radius:2px; }
QCheckBox::indicator:checked { background:#3b75ac; border:1px solid #5f95c7; border-radius:2px; image:url(:/ui/check.svg); }
QCheckBox::indicator:disabled { background:#30353e; border-color:#4b525e; }
QListWidget,QTableView { background:#1e2229; alternate-background-color:#252a32; border:1px solid #3d434e; border-radius:3px; padding:2px; outline:0; }
QListWidget::item { padding:5px 7px; border-radius:2px; }
QListWidget::item:hover { background:#303846; }
QListWidget::item:selected { background:#344d69; color:#f0f6fd; }
QListWidget#settingsNav { background:#282d36; }
QHeaderView::section { background:#303641; color:#bfc9d8; border:0; border-right:1px solid #454b56; padding:5px; }
QTabWidget::pane { border:1px solid #3b434f; }
QTabBar::tab { background:#272b32; padding:5px 8px; border-bottom:2px solid transparent; }
QTabBar::tab:selected { background:#313b48; border-bottom-color:#6b9aca; }
QScrollArea { border:0; background:transparent; }
QScrollArea>QWidget>QWidget { background:#252931; }
QScrollBar:vertical { width:8px; background:#242830; margin:1px 0; }
QScrollBar::handle:vertical { background:#4f5969; border-radius:2px; min-height:24px; }
QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical { height:0; }
QScrollBar:horizontal { height:8px; background:#242830; }
QScrollBar::handle:horizontal { background:#4f5969; border-radius:2px; min-width:26px; }
QScrollBar::add-line:horizontal,QScrollBar::sub-line:horizontal { width:0; }
QScrollBar::add-page,QScrollBar::sub-page { background:transparent; }
QSplitter::handle { background:#414957; }
QSplitter::handle:hover { background:#6a8eb4; }
QGroupBox { background:#272c34; border:1px solid #414852; border-radius:3px; margin-top:12px; padding:6px; }
QGroupBox::title { subcontrol-origin:margin; left:7px; padding:0 4px; color:#c9d1dc; font-weight:600; }
QMenuBar,QMenu { background:#282d34; color:#e5e9ef; }
QMenu { border:1px solid #484f5d; border-radius:3px; padding:3px; }
QMenu::item { padding:5px 16px 5px 8px; }
QMenuBar::item:selected,QMenu::item:selected { background:#355779; color:#ffffff; }
QMenu::separator { height:1px; background:#444b56; margin:3px 5px; }
QStatusBar { background:#22262d; border-top:1px solid #3b414a; }
QStatusBar::item { border:0; }
QToolTip { color:#eaf0f8; background:#343c4a; border:1px solid #536276; padding:4px 6px; }
QToolBar { background:#292e36; border:1px solid #485361; border-radius:3px; padding:3px; spacing:2px; }
)qss");
	// The capture UI deliberately uses physical desktop coordinates. Scale its
	// visual design units for the system DPI without changing capture geometry.
	QRegularExpression pixels("(\\d+)px"); auto matches = pixels.globalMatch(sheet); QList<QRegularExpressionMatch> replacements;
	while (matches.hasNext()) replacements.append(matches.next());
	for (int i = replacements.size() - 1; i >= 0; --i) { auto match = replacements[i]; sheet.replace(match.capturedStart(), match.capturedLength(), QString::number(Px(match.captured(1).toInt())) + "px"); }
	app.setStyleSheet(sheet);
}
QPushButton* Toggle(const QString& icon, const QString& label, QWidget* parent)
{
	auto button = new QPushButton(parent);
	button->setIcon(Icon(icon));
	button->setCheckable(true);
	button->setFixedWidth(Px(38));
	button->setAccessibleName(label);
	button->setToolTip(label + Ui("：关"));
	button->setStyleSheet("QPushButton{padding:0;}");
	QObject::connect(button, &QPushButton::toggled, button, [button, label](bool on) {
		button->setToolTip(label + (on ? Ui("：开") : Ui("：关")));
	});
	return button;
}
void PolishButtons(QWidget* root)
{
	for (auto button : root->findChildren<QAbstractButton*>())
	{
		if (!qobject_cast<QPushButton*>(button)) continue;
		button->setCursor(Qt::PointingHandCursor);
		if (!button->icon().isNull()) { button->setIconSize(QSize(Px(16), Px(16))); continue; }
		QString label = button->text(), icon;
		auto contains = [&](const char *source) { return label.contains(Ui(source), Qt::CaseInsensitive); };
		if (contains("摄像头")) icon = "camera";
		else if (contains("设置")) icon = "settings";
		else if (contains("讲解") || contains("颜色")) icon = "pen";
		else if (contains("导出") || contains("字幕")) icon = "export";
		else if (contains("剪辑") || contains("裁剪") || contains("拆分")) icon = "edit";
		else if (contains("文件位置") || contains("打开") || label == Ui("选择")) icon = "folder";
		else if (contains("刷新")) icon = "refresh";
		else if (contains("图片")) icon = "image";
		else if (contains("全屏") || contains("区域")) icon = "full";
		if (!icon.isEmpty()) { button->setIcon(Icon(icon)); button->setIconSize(QSize(Px(16), Px(16))); }
		button->setCursor(Qt::PointingHandCursor);
	}
}
}
