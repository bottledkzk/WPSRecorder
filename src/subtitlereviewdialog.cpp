#include "subtitlereviewdialog.h"

#include "speechservices.h"
#include "subtitledialog.h"
#include "uilanguage.h"
#include "uitheme.h"

#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

bool ShowSubtitleReview(KRRSubtitleDialog* parent, const KRREditProject& project, int currentRow,
	const QString& target, KRREditProject& result, int& firstChanged)
{
	QDialog dialog(parent);
	dialog.setObjectName("subtitleReview");
	dialog.setWindowTitle(Ui("AI 校对字幕与配音稿"));
	dialog.setWindowFlag(Qt::WindowContextHelpButtonHint, false);
	dialog.resize(RecorderUi::Px(820), RecorderUi::Px(590));
	auto layout = new QVBoxLayout(&dialog);
	auto form = new QFormLayout;
	auto range = new QHBoxLayout;
	auto first = new QSpinBox(&dialog);
	auto last = new QSpinBox(&dialog);
	first->setObjectName("reviewFirstLine");
	last->setObjectName("reviewLastLine");
	for (auto field : {first, last})
	{
		field->setRange(1, project.captions.size());
		field->setValue(currentRow + 1);
		range->addWidget(field);
		if (field == first) range->addWidget(new QLabel(Ui("至"), &dialog));
	}
	auto whole = new QPushButton(Ui("选全文"), &dialog);
	whole->setEnabled(project.captions.size() <= 96);
	range->addWidget(whole);
	range->addStretch();
	form->addRow(Ui("起止句编号"), range);
	auto goal = new QLineEdit(&dialog);
	goal->setObjectName("reviewGoal");
	goal->setMaxLength(600);
	goal->setPlaceholderText(Ui("例如：统一产品名称，检查数字和单位"));
	form->addRow(Ui("审核要求"), goal);
	layout->addLayout(form);
	auto service = new QLabel(Ui("将字幕、配音稿和术语发往 %1。译文目标：%2。")
		.arg(SpeechServiceHost("assistant"), target == "en" ? "English" : Ui("中文")), &dialog);
	service->setWordWrap(true);
	layout->addWidget(service);

	auto table = new QTableWidget(0, 3, &dialog);
	table->setObjectName("reviewSuggestions");
	table->setHorizontalHeaderLabels({Ui("采纳"), Ui("原片时间"), Ui("建议依据")});
	table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
	table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
	table->setSelectionBehavior(QAbstractItemView::SelectRows);
	table->setSelectionMode(QAbstractItemView::SingleSelection);
	table->setEditTriggers(QAbstractItemView::NoEditTriggers);
	layout->addWidget(table, 1);
	auto detail = new QPlainTextEdit(&dialog);
	detail->setObjectName("reviewBeforeAfter");
	detail->setReadOnly(true);
	detail->setPlaceholderText(Ui("选中建议，查看修改前后。"));
	layout->addWidget(detail, 1);
	auto status = new QLabel(Ui("默认审核当前句。一次最多 96 句。"), &dialog);
	status->setObjectName("reviewStatus");
	status->setTextFormat(Qt::PlainText);
	status->setWordWrap(true);
	layout->addWidget(status);

	auto footer = new QHBoxLayout;
	auto run = new QPushButton(Ui("开始审核"), &dialog);
	run->setObjectName("runSubtitleReview");
	auto acceptAll = new QPushButton(Ui("勾选全部"), &dialog);
	auto cancel = new QPushButton(Ui("取消"), &dialog);
	auto apply = new QPushButton(Ui("采纳选中建议"), &dialog);
	apply->setObjectName("applySubtitleSuggestions");
	apply->setProperty("role", "primary");
	for (auto* button : {run, acceptAll}) footer->addWidget(button);
	footer->addStretch();
	footer->addWidget(cancel);
	footer->addWidget(apply);
	layout->addLayout(footer);
	RecorderUi::PolishButtons(&dialog);

	KRRSubtitleReview review;
	KRRMediaEditor worker;
	const auto update = [&](bool running)
	{
		run->setEnabled(!running);
		acceptAll->setEnabled(!running && table->rowCount() > 0);
		bool checked = false;
		for (int i = 0; i < table->rowCount(); ++i) checked |= table->item(i, 0)->checkState() == Qt::Checked;
		apply->setEnabled(!running && checked);
	};
	update(false);
	QObject::connect(parent, &KRRSubtitleDialog::reviewInterrupted, &dialog, &QDialog::reject);
	QObject::connect(&dialog, &QDialog::rejected, &worker, &KRRMediaEditor::cancel);
	QObject::connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
	QObject::connect(whole, &QPushButton::clicked, &dialog, [&] { first->setValue(1); last->setValue(project.captions.size()); });
	QObject::connect(run, &QPushButton::clicked, &dialog, [&]
	{
		if (last->value() < first->value() || last->value() - first->value() >= 96)
		{
			status->setText(Ui("检查审核范围和译文语言。每次最多审核 96 句。"));
			return;
		}
		if (!worker.reviewSubtitles(project, first->value() - 1, last->value() - 1, target, goal->text().trimmed())) return;
		table->setRowCount(0);
		detail->clear();
		update(true);
		status->setText(Ui("正在审核字幕…"));
	});
	QObject::connect(table, &QTableWidget::itemChanged, &dialog, [&](QTableWidgetItem* item)
	{
		if (item->column() == 0) update(worker.isBusy());
	});
	QObject::connect(table, &QTableWidget::currentCellChanged, &dialog, [&](int row)
	{
		if (row < 0) { detail->clear(); return; }
		const auto& change = review.suggestions[row];
		const auto& line = project.captions[change.row];
		const auto pair = [](const QString& name, const QString& before, const QString& after)
		{
			return "\n\n" + Ui("%1 修改前").arg(name) + "\n" + (before.isEmpty() ? QString("—") : before) +
				"\n" + Ui("%1 建议").arg(name) + "\n" + after;
		};
		auto description = Ui("原文") + "\n" + line.text;
		if (change.translation) description += pair(Ui("译文"), line.translation, *change.translation);
		if (change.narration) description += pair(Ui("配音稿"), line.narrationText, *change.narration);
		detail->setPlainText(description);
	});
	QObject::connect(&worker, &KRRMediaEditor::progress, &dialog, [status](const QString& message) { status->setText(message); });
	QObject::connect(&worker, &KRRMediaEditor::subtitlesReviewed, &dialog, [&](const KRRSubtitleReview& received)
	{
		review = received;
		QSignalBlocker block(table);
		table->setRowCount(review.suggestions.size());
		for (int i = 0; i < review.suggestions.size(); ++i)
		{
			const auto& change = review.suggestions[i];
			auto choice = new QTableWidgetItem(QString::number(change.row + 1));
			choice->setCheckState(Qt::Unchecked);
			table->setItem(i, 0, choice);
			table->setItem(i, 1, new QTableWidgetItem(EditTime(project.captions[change.row].begin)));
			auto evidence = new QTableWidgetItem(change.reason);
			evidence->setToolTip(change.reason.toHtmlEscaped());
			table->setItem(i, 2, evidence);
		}
	});
	QObject::connect(&worker, &KRRMediaEditor::completed, &dialog, [&](const QString&, const QString& error)
	{
		update(false);
		if (table->rowCount() > 0) table->setCurrentCell(0, 2);
		status->setText(!error.isEmpty() ? UiDiagnostic(error) : table->rowCount() ?
			Ui("有 %1 条建议。勾选要采纳的修改。").arg(table->rowCount()) : Ui("这一范围没有修改建议。"));
	});
	QObject::connect(acceptAll, &QPushButton::clicked, &dialog, [&]
	{
		for (int i = 0; i < table->rowCount(); ++i) table->item(i, 0)->setCheckState(Qt::Checked);
	});
	QObject::connect(apply, &QPushButton::clicked, &dialog, [&]
	{
		QVector<int> selected;
		for (int i = 0; i < table->rowCount(); ++i)
			if (table->item(i, 0)->checkState() == Qt::Checked) selected.append(i);
		QString error;
		if (!ApplySubtitleSuggestions(project, review, selected, result, error)) { status->setText(error); return; }
		firstChanged = review.suggestions[selected.first()].row;
		dialog.accept();
	});
	return dialog.exec() == QDialog::Accepted;
}
