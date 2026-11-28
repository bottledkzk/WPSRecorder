#ifndef WPSRECORDER_EMBEDDEDCLI_H
#define WPSRECORDER_EMBEDDEDCLI_H

#include <QProcess>
#include <QStringList>

inline constexpr char EmbeddedCliEnvironment[] = "WPSRECORDER_EMBEDDED_CLI";

inline void ConfigureEmbeddedCli(QProcess& process)
{
	auto environment = QProcessEnvironment::systemEnvironment();
	environment.insert(EmbeddedCliEnvironment, "1");
	process.setProcessEnvironment(environment);
}

inline bool EmbeddedCliAllows(const QStringList& arguments)
{
	for (const auto& argument : arguments)
	{
		const auto name = argument.section('=', 0, 0);
		if (name == "--whisper" || name == "--serve") return false;
	}
	return true;
}

#endif
