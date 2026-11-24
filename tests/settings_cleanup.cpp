// Remove only history entries under explicitly supplied QA roots; preserve all other settings.
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
int main(int argc,char **argv)
{
    QCoreApplication app(argc,argv);auto args=app.arguments();if(args.size()<5)return 2;
    auto normalized=[](const QString &s){return QDir::cleanPath(QDir::fromNativeSeparators(s));};
    auto isTest=[&](const QString &path){for(int i=4;i<args.size();++i)if(normalized(path).startsWith(normalized(args[i])+"/",Qt::CaseInsensitive))return true;return false;};
    QSettings settings(args[1],QSettings::IniFormat);settings.setIniCodec("UTF-8");
    auto recent=settings.value("recent").toStringList();QStringList kept;for(auto path:recent)if(!isTest(path))kept<<path;
    int removed=recent.size()-kept.size();bool clear=isTest(settings.value("lastOutput").toString());bool restore=QFileInfo(args[3]).isFile()&&!isTest(args[3]);
    if(restore&&(removed||clear)&&!kept.contains(args[3]))kept.prepend(args[3]);
    if(kept!=recent)settings.setValue("recent",kept);if(clear)settings.setValue("lastOutput",restore?args[3]:QString());settings.sync();
    QJsonObject report{{"passed",settings.status()==QSettings::NoError},{"removed_test_history_entries",removed},{"preserved_entries",kept.size()},{"last_output_test_reference_cleared",clear},{"existing_user_record_restored_if_needed",restore&&(removed||clear)},{"other_settings_unchanged",true}};
    QFile file(args[2]);if(!file.open(QIODevice::WriteOnly))return 1;file.write(QJsonDocument(report).toJson());return settings.status()==QSettings::NoError?0:1;
}
