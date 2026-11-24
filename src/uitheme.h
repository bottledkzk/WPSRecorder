#ifndef WPSRECORDER_UITHEME_H
#define WPSRECORDER_UITHEME_H
#include <QColor>
#include <QIcon>
#include <QString>
class QApplication;
class QWidget;
class QPushButton;
class QTableWidget;
class QJsonObject;
namespace RecorderUi
{
int Px(int value);
QIcon Icon(const QString& name);
struct ToolbarTheme
{
	QColor background, border, text, field, divider, hover, selected, selectedBorder, muted, record;
	bool isLight() const { return background.isValid(); }
	QIcon icon(const QString& name) const;
};
ToolbarTheme ReadToolbarTheme();
bool WriteToolbarTheme(const QJsonObject& data, ToolbarTheme& theme);
void Apply(QApplication& application);
QPushButton* Toggle(const QString& icon, const QString& label, QWidget* parent);
void PolishButtons(QWidget* root);
void FitTableColumn(QTableWidget* table, int column, const QString& sample = {});
}
#endif
