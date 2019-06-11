/**************************************************************************
**
** Copyright (C) 2017 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the Qt Installer Framework.
**
** $QT_BEGIN_LICENSE:GPL-EXCEPT$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
**************************************************************************/
#include "settings.h"

#include "errors.h"
#include "qinstallerglobal.h"
#include "repository.h"

#include <QtCore/QFileInfo>
#include <QtCore/QStringList>
#include <QtGui/QFontMetrics>
#include <QtWidgets/QApplication>

#include <QRegularExpression>
#include <QXmlStreamReader>

using namespace QInstaller;

static const QLatin1String scInstallerApplicationIcon("InstallerApplicationIcon");
static const QLatin1String scInstallerWindowIcon("InstallerWindowIcon");
static const QLatin1String scLogo("Logo");
static const QLatin1String scPrefix("Prefix");
static const QLatin1String scWatermark("Watermark");
static const QLatin1String scBanner("Banner");
static const QLatin1String scProductUrl("ProductUrl");
static const QLatin1String scBackground("Background");
static const QLatin1String scAdminTargetDir("AdminTargetDir");
static const QLatin1String scMaintenanceToolName("MaintenanceToolName");
static const QLatin1String scVirtualRepositories("VirtualRepositories");
static const QLatin1String scUserRepositories("UserRepositories");
static const QLatin1String scTmpRepositories("TemporaryRepositories");
static const QLatin1String scMaintenanceToolIniFile("MaintenanceToolIniFile");
static const QLatin1String scRemoteRepositories("RemoteRepositories");
static const QLatin1String scDependsOnLocalInstallerBinary("DependsOnLocalInstallerBinary");
static const QLatin1String scTranslations("Translations");
static const QLatin1String scCreateLocalRepository("CreateLocalRepository");
static const QLatin1String scInstallActionColumnVisible("InstallActionColumnVisible");

static const QLatin1String scFtpProxy("FtpProxy");
static const QLatin1String scHttpProxy("HttpProxy");
static const QLatin1String scProxyType("ProxyType");

const char scControlScript[] = "ControlScript";

template <typename T>
static QSet<T> variantListToSet(const QVariantList &list)
{
    QSet<T> set;
    foreach (const QVariant &variant, list)
        set.insert(variant.value<T>());
    return set;
}

static void raiseError(QXmlStreamReader &reader, const QString &error, Settings::ParseMode parseMode)
{
    if (parseMode == Settings::StrictParseMode) {
        reader.raiseError(error);
    } else {
        QFile *xmlFile = qobject_cast<QFile*>(reader.device());
        if (xmlFile) {
            qWarning().noquote().nospace()
                    << "Ignoring following settings reader error in " << xmlFile->fileName()
                                 << ", line " << reader.lineNumber() << ", column " << reader.columnNumber()
                                 << ": " << error;
        } else {
            qWarning("Ignoring following settings reader error: %s", qPrintable(error));
        }
    }
}

static QStringList readArgumentAttributes(QXmlStreamReader &reader, Settings::ParseMode parseMode,
                                          const QString &tagName, bool lc = false)
{
    QStringList arguments;

    while (QXmlStreamReader::TokenType token = reader.readNext()) {
        switch (token) {
            case QXmlStreamReader::StartElement: {
                if (!reader.attributes().isEmpty()) {
                    raiseError(reader, QString::fromLatin1("Unexpected attribute for element \"%1\".")
                        .arg(reader.name().toString()), parseMode);
                    return arguments;
                } else {
                    if (reader.name().toString() == tagName) {
                        (lc) ? arguments.append(reader.readElementText().toLower()) :
                               arguments.append(reader.readElementText());
                    } else {
                        raiseError(reader, QString::fromLatin1("Unexpected element \"%1\".").arg(reader.name()
                            .toString()), parseMode);
                        return arguments;
                    }
                }
            }
            break;
            case QXmlStreamReader::Characters: {
                if (reader.isWhitespace())
                    continue;
                arguments.append(reader.text().toString().split(QRegularExpression(QLatin1String("\\s+")),
                    QString::SkipEmptyParts));
            }
            break;
            case QXmlStreamReader::EndElement: {
                return arguments;
            }
            default:
            break;
        }
    }
    return arguments;
}

static QSet<Repository> readRepositories(QXmlStreamReader &reader, bool isDefault, Settings::ParseMode parseMode)
{
    QSet<Repository> set;
    while (reader.readNextStartElement()) {
        if (reader.name() == QLatin1String("Repository")) {
            Repository repo(QString(), isDefault);
            while (reader.readNextStartElement()) {
                if (reader.name() == QLatin1String("Url")) {
                    repo.setUrl(reader.readElementText());
                } else if (reader.name() == QLatin1String("Username")) {
                    repo.setUsername(reader.readElementText());
                } else if (reader.name() == QLatin1String("Password")) {
                    repo.setPassword(reader.readElementText());
                } else if (reader.name() == QLatin1String("DisplayName")) {
                    repo.setDisplayName(reader.readElementText());
                } else if (reader.name() == QLatin1String("Enabled")) {
                    repo.setEnabled(bool(reader.readElementText().toInt()));
                } else {
                    raiseError(reader, QString::fromLatin1("Unexpected element \"%1\".").arg(reader.name()
                        .toString()), parseMode);
                }

                if (!reader.attributes().isEmpty()) {
                    raiseError(reader, QString::fromLatin1("Unexpected attribute for element \"%1\".")
                        .arg(reader.name().toString()), parseMode);
                }
            }
            set.insert(repo);
        } else {
            raiseError(reader, QString::fromLatin1("Unexpected element \"%1\".").arg(reader.name().toString()),
                parseMode);
        }

        if (!reader.attributes().isEmpty()) {
            raiseError(reader, QString::fromLatin1("Unexpected attribute for element \"%1\".").arg(reader
                .name().toString()), parseMode);
        }
    }
    return set;
}


// -- Settings::Private

class Settings::Private : public QSharedData
{
public:
    Private()
        : m_replacementRepos(false)
    {}

    QVariantHash m_data;
    bool m_replacementRepos;

    QString absolutePathFromKey(const QString &key, const QString &suffix = QString()) const
    {
        const QString value = m_data.value(key).toString();
        if (value.isEmpty())
            return QString();

        const QString path = value + suffix;
        if (QFileInfo(path).isAbsolute())
            return path;
        return m_data.value(scPrefix).toString() + QLatin1String("/") + path;
    }
};


// -- Settings

Settings::Settings()
    : d(new Private)
{
}

Settings::~Settings()
{
}

Settings::Settings(const Settings &other)
    : d(other.d)
{
}

Settings& Settings::operator=(const Settings &other)
{
    Settings copy(other);
    std::swap(d, copy.d);
    return *this;
}

/* static */
Settings Settings::fromFileAndPrefix(const QString &path, const QString &prefix, ParseMode parseMode)
{
    QFile file(path);
    QFile overrideConfig(QLatin1String(":/overrideconfig.xml"));

    if (overrideConfig.exists())
        file.setFileName(overrideConfig.fileName());

    if (!file.open(QIODevice::ReadOnly))
        throw Error(tr("Cannot open settings file %1 for reading: %2").arg(path, file.errorString()));

    QXmlStreamReader reader(&file);
    if (reader.readNextStartElement()) {
        if (reader.name() != QLatin1String("Installer")) {
            reader.raiseError(QString::fromLatin1("Unexpected element \"%1\" as root element.").arg(reader
                .name().toString()));
        }
    }
    QStringList elementList;
    elementList << scName << scVersion << scTitle << scPublisher << scProductUrl
                << scTargetDir << scAdminTargetDir
                << scInstallerApplicationIcon << scInstallerWindowIcon
                << scLogo << scWatermark << scBanner << scBackground
                << scStartMenuDir << scMaintenanceToolName << scMaintenanceToolIniFile << scRemoveTargetDir
                << scRunProgram << scRunProgramArguments << scRunProgramDescription
                << scDependsOnLocalInstallerBinary
                << scAllowSpaceInPath << scAllowNonAsciiCharacters << scDisableAuthorizationFallback
                << scWizardStyle << scStyleSheet << scTitleColor
                << scWizardDefaultWidth << scWizardDefaultHeight
                << scRepositorySettingsPageVisible << scTargetConfigurationFile
                << scRemoteRepositories << scTranslations << scUrlQueryString << QLatin1String(scControlScript)
                << scCreateLocalRepository << scInstallActionColumnVisible << scSupportsModify;

    Settings s;
    s.d->m_data.insert(scPrefix, prefix);
    while (reader.readNextStartElement()) {
        const QString name = reader.name().toString();
        if (!elementList.contains(name))
            raiseError(reader, QString::fromLatin1("Unexpected element \"%1\".").arg(name), parseMode);

        if (!reader.attributes().isEmpty()) {
            raiseError(reader, QString::fromLatin1("Unexpected attribute for element \"%1\".").arg(name),
                parseMode);
        }

        if (s.d->m_data.contains(name))
            reader.raiseError(QString::fromLatin1("Element \"%1\" has been defined before.").arg(name));

        if (name == scTranslations) {
            s.setTranslations(readArgumentAttributes(reader, parseMode, QLatin1String("Translation"), true));
        } else if (name == scRunProgramArguments) {
            s.setRunProgramArguments(readArgumentAttributes(reader, parseMode, QLatin1String("Argument")));
        } else if (name == scRemoteRepositories) {
            s.addDefaultRepositories(readRepositories(reader, true, parseMode));
        } else {
            s.d->m_data.insert(name, reader.readElementText(QXmlStreamReader::SkipChildElements));
        }
    }

    if (reader.error() != QXmlStreamReader::NoError) {
        throw Error(QString::fromLatin1("Error in %1, line %2, column %3: %4").arg(path).arg(reader
            .lineNumber()).arg(reader.columnNumber()).arg(reader.errorString()));
    }

    if (s.d->m_data.value(scName).isNull())
        throw Error(QString::fromLatin1("Missing or empty <Name> tag in %1.").arg(file.fileName()));
    if (s.d->m_data.value(scVersion).isNull())
        throw Error(QString::fromLatin1("Missing or empty <Version> tag in %1.").arg(file.fileName()));

    // Add some possible missing values
    if (!s.d->m_data.contains(scInstallerApplicationIcon))
        s.d->m_data.insert(scInstallerApplicationIcon, QLatin1String(":/installer"));
    if (!s.d->m_data.contains(scInstallerWindowIcon)) {
        s.d->m_data.insert(scInstallerWindowIcon,
                           QString(QLatin1String(":/installer") + s.systemIconSuffix()));
    }
    if (!s.d->m_data.contains(scRemoveTargetDir))
        s.d->m_data.insert(scRemoveTargetDir, scTrue);
    if (s.d->m_data.value(scMaintenanceToolName).toString().isEmpty()) {
        s.d->m_data.insert(scMaintenanceToolName,
            // TODO: Remove deprecated 'UninstallerName'.
            s.d->m_data.value(QLatin1String("UninstallerName"), QLatin1String("maintenancetool"))
            .toString());
    }
    if (s.d->m_data.value(scTargetConfigurationFile).toString().isEmpty())
        s.d->m_data.insert(scTargetConfigurationFile, QLatin1String("components.xml"));
    if (s.d->m_data.value(scMaintenanceToolIniFile).toString().isEmpty()) {
        s.d->m_data.insert(scMaintenanceToolIniFile,
            // TODO: Remove deprecated 'UninstallerIniFile'.
            s.d->m_data.value(QLatin1String("UninstallerIniFile"), QString(s.maintenanceToolName()
            + QLatin1String(".ini"))).toString());
    }
    if (!s.d->m_data.contains(scDependsOnLocalInstallerBinary))
        s.d->m_data.insert(scDependsOnLocalInstallerBinary, false);
    if (!s.d->m_data.contains(scRepositorySettingsPageVisible))
        s.d->m_data.insert(scRepositorySettingsPageVisible, true);
    if (!s.d->m_data.contains(scCreateLocalRepository))
        s.d->m_data.insert(scCreateLocalRepository, false);
    if (!s.d->m_data.contains(scInstallActionColumnVisible))
        s.d->m_data.insert(scInstallActionColumnVisible, false);

    return s;
}

QString Settings::logo() const
{
    return d->absolutePathFromKey(scLogo);
}

QString Settings::title() const
{
    return d->m_data.value(scTitle).toString();
}

QString Settings::applicationName() const
{
    return d->m_data.value(scName).toString();
}

QString Settings::version() const
{
    return d->m_data.value(scVersion).toString();
}

QString Settings::publisher() const
{
    return d->m_data.value(scPublisher).toString();
}

QString Settings::url() const
{
    return d->m_data.value(scProductUrl).toString();
}

QString Settings::watermark() const
{
    return d->absolutePathFromKey(scWatermark);
}

QString Settings::banner() const
{
    return d->absolutePathFromKey(scBanner);
}

QString Settings::background() const
{
    return d->absolutePathFromKey(scBackground);
}

QString Settings::wizardStyle() const
{
    return d->m_data.value(scWizardStyle).toString();
}

QString Settings::styleSheet() const
{
    return d->absolutePathFromKey(scStyleSheet);
}

QString Settings::titleColor() const
{
    return d->m_data.value(scTitleColor).toString();
}

static int lengthToInt(const QVariant &variant)
{
    QString length = variant.toString().trimmed();
    if (length.endsWith(QLatin1String("em"), Qt::CaseInsensitive)) {
        length.chop(2);
        return qRound(length.toDouble() * QApplication::fontMetrics().height());
    }
    if (length.endsWith(QLatin1String("ex"), Qt::CaseInsensitive)) {
        length.chop(2);
        return qRound(length.toDouble() * QApplication::fontMetrics().xHeight());
    }
    if (length.endsWith(QLatin1String("px"), Qt::CaseInsensitive)) {
        length.chop(2);
    }
    return length.toInt();
}

int Settings::wizardDefaultWidth() const
{
    return lengthToInt(d->m_data.value(scWizardDefaultWidth));
}

int Settings::wizardDefaultHeight() const
{
    return lengthToInt(d->m_data.value(scWizardDefaultHeight));
}

QString Settings::installerApplicationIcon() const
{
    return d->absolutePathFromKey(scInstallerApplicationIcon, systemIconSuffix());
}

QString Settings::installerWindowIcon() const
{
    return d->absolutePathFromKey(scInstallerWindowIcon);
}

QString Settings::systemIconSuffix() const
{
#if defined(Q_OS_OSX)
    return QLatin1String(".icns");
#elif defined(Q_OS_WIN)
    return QLatin1String(".ico");
#endif
    return QLatin1String(".png");
}


QString Settings::removeTargetDir() const
{
    return d->m_data.value(scRemoveTargetDir).toString();
}

QString Settings::maintenanceToolName() const
{
    return d->m_data.value(scMaintenanceToolName).toString();
}

QString Settings::maintenanceToolIniFile() const
{
    return d->m_data.value(scMaintenanceToolIniFile).toString();
}

QString Settings::runProgram() const
{
    return d->m_data.value(scRunProgram).toString();
}

QStringList Settings::runProgramArguments() const
{
    const QVariant variant = d->m_data.values(scRunProgramArguments);
    if (variant.canConvert<QStringList>())
        return variant.value<QStringList>();
    return QStringList();
}

void Settings::setRunProgramArguments(const QStringList &arguments)
{
    d->m_data.insert(scRunProgramArguments, arguments);
}


QString Settings::runProgramDescription() const
{
    return d->m_data.value(scRunProgramDescription).toString();
}

QString Settings::startMenuDir() const
{
    return d->m_data.value(scStartMenuDir).toString();
}

QString Settings::targetDir() const
{
    return d->m_data.value(scTargetDir).toString();
}

QString Settings::adminTargetDir() const
{
    return d->m_data.value(scAdminTargetDir).toString();
}

QString Settings::configurationFileName() const
{
    return d->m_data.value(scTargetConfigurationFile).toString();
}

bool Settings::createLocalRepository() const
{
    return d->m_data.value(scCreateLocalRepository).toBool();
}

bool Settings::installActionColumnVisible() const
{
    return d->m_data.value(scInstallActionColumnVisible, false).toBool();
}

bool Settings::allowSpaceInPath() const
{
    return d->m_data.value(scAllowSpaceInPath, true).toBool();
}

bool Settings::allowNonAsciiCharacters() const
{
    return d->m_data.value(scAllowNonAsciiCharacters, false).toBool();
}

bool Settings::disableAuthorizationFallback() const
{
    return d->m_data.value(scDisableAuthorizationFallback, false).toBool();
}

bool Settings::dependsOnLocalInstallerBinary() const
{
    return d->m_data.value(scDependsOnLocalInstallerBinary).toBool();
}

bool Settings::hasReplacementRepos() const
{
    return d->m_replacementRepos;
}

QSet<Repository> Settings::repositories() const
{
    if (d->m_replacementRepos) {
        return variantListToSet<Repository>(d->m_data.values(scVirtualRepositories)
            + d->m_data.values(scTmpRepositories));
    }

    return variantListToSet<Repository>(d->m_data.values(scRepositories)
        + d->m_data.values(scVirtualRepositories)
        + d->m_data.values(scUserRepositories) + d->m_data.values(scTmpRepositories));
}

QSet<Repository> Settings::defaultRepositories() const
{
    return variantListToSet<Repository>(d->m_data.values(scRepositories));
}

void Settings::setDefaultRepositories(const QSet<Repository> &repositories)
{
    d->m_data.remove(scRepositories);
    addDefaultRepositories(repositories);
}

void Settings::addDefaultRepositories(const QSet<Repository> &repositories)
{
    foreach (const Repository &repository, repositories)
        d->m_data.insertMulti(scRepositories, QVariant().fromValue(repository));
}

static bool apply(const RepoHash &updates, QHash<QUrl, Repository> *reposToUpdate)
{
    bool update = false;
    QList<QPair<Repository, Repository> > values = updates.values(QLatin1String("replace"));
    for (int a = 0; a < values.count(); ++a) {
        QPair<Repository, Repository> data = values.at(a);
        if (reposToUpdate->contains(data.first.url())) {
            update = true;
            const Repository oldRepo = reposToUpdate->take(data.first.url());
            data.second.setEnabled(oldRepo.isEnabled() && data.second.isEnabled());
            reposToUpdate->insert(data.second.url(), data.second);
        }
    }

    values = updates.values(QLatin1String("remove"));
    for (int a = 0; a < values.count(); ++a) {
        const QPair<Repository, Repository> data = values.at(a);
        if (reposToUpdate->contains(data.first.url())) {
            update = true;
            reposToUpdate->remove(data.first.url());
        }
    }

    values = updates.values(QLatin1String("add"));
    for (int a = 0; a < values.count(); ++a) {
        const QPair<Repository, Repository> data = values.at(a);
        if (!reposToUpdate->contains(data.first.url())) {
            update = true;
            reposToUpdate->insert(data.first.url(), data.first);
        }
    }
    return update;
}

Settings::Update Settings::updateDefaultRepositories(const RepoHash &updates)
{
    if (updates.isEmpty())
        return Settings::NoUpdatesApplied;

    QHash <QUrl, Repository> defaultRepos;
    foreach (const QVariant &variant, d->m_data.values(scRepositories)) {
        const Repository repository = variant.value<Repository>();
        defaultRepos.insert(repository.url(), repository);
    }

    const bool updated = apply(updates, &defaultRepos);
    if (updated)
        setDefaultRepositories(defaultRepos.values().toSet());
    return updated ? Settings::UpdatesApplied : Settings::NoUpdatesApplied;
}

QSet<Repository> Settings::temporaryRepositories() const
{
    return variantListToSet<Repository>(d->m_data.values(scTmpRepositories));
}

void Settings::setTemporaryRepositories(const QSet<Repository> &repositories, bool replace)
{
    d->m_data.remove(scTmpRepositories);
    addTemporaryRepositories(repositories, replace);
}

void Settings::addTemporaryRepositories(const QSet<Repository> &repositories, bool replace)
{
    d->m_replacementRepos = replace;
    foreach (const Repository &repository, repositories)
        d->m_data.insertMulti(scTmpRepositories, QVariant().fromValue(repository));
}

QSet<Repository> Settings::userRepositories() const
{
    return variantListToSet<Repository>(d->m_data.values(scUserRepositories));
}

void Settings::setUserRepositories(const QSet<Repository> &repositories)
{
    d->m_data.remove(scUserRepositories);
    addUserRepositories(repositories);
}

void Settings::addUserRepositories(const QSet<Repository> &repositories)
{
    foreach (const Repository &repository, repositories)
        d->m_data.insertMulti(scUserRepositories, QVariant().fromValue(repository));
}

Settings::Update Settings::updateUserRepositories(const RepoHash &updates)
{
    if (updates.isEmpty())
        return Settings::NoUpdatesApplied;

    QHash <QUrl, Repository> reposToUpdate;
    foreach (const QVariant &variant, d->m_data.values(scUserRepositories)) {
        const Repository repository = variant.value<Repository>();
        reposToUpdate.insert(repository.url(), repository);
    }

    const bool updated = apply(updates, &reposToUpdate);
    if (updated)
        setUserRepositories(reposToUpdate.values().toSet());
    return updated ? Settings::UpdatesApplied : Settings::NoUpdatesApplied;
}

QSet<Repository> Settings::virtualRepositories() const
{
    return variantListToSet<Repository>(d->m_data.values(scVirtualRepositories));
}

void Settings::setVirtualRepositories(const QSet<Repository> &repositories)
{
    d->m_data.remove(scVirtualRepositories);
    addVirtualRepositories(repositories);
}

void Settings::addVirtualRepositories(const QSet<Repository> &repositories)
{
    foreach (const Repository &repository, repositories)
        d->m_data.insertMulti(scVirtualRepositories, QVariant().fromValue(repository));
}

void Settings::removeVirtualRepositories(const QSet<Repository> &repositories)
{
    QHash <QUrl, Repository> virtualRepos;
    foreach (const QVariant &variant, d->m_data.values(scVirtualRepositories)) {
        const Repository repository = variant.value<Repository>();
        virtualRepos.insert(repository.url(), repository);
    }

    bool removed = false;
    foreach (const Repository &repository, repositories) {
        virtualRepos.remove(repository.url());
        removed = true;
    }

    if (removed)
        setVirtualRepositories(virtualRepos.values().toSet());
}

bool Settings::containsValue(const QString &key) const
{
    return d->m_data.contains(key);
}

QVariant Settings::value(const QString &key, const QVariant &defaultValue) const
{
    return d->m_data.value(key, defaultValue);
}

QVariantList Settings::values(const QString &key, const QVariantList &defaultValue) const
{
    QVariantList list = d->m_data.values(key);
    return list.isEmpty() ? defaultValue : list;
}

bool Settings::repositorySettingsPageVisible() const
{
    return d->m_data.value(scRepositorySettingsPageVisible, true).toBool();
}

void Settings::setRepositorySettingsPageVisible(bool visible)
{
    d->m_data.insert(scRepositorySettingsPageVisible, visible);
}

Settings::ProxyType Settings::proxyType() const
{
    return Settings::ProxyType(d->m_data.value(scProxyType, Settings::NoProxy).toInt());
}

void Settings::setProxyType(Settings::ProxyType type)
{
    d->m_data.insert(scProxyType, type);
}

QNetworkProxy Settings::ftpProxy() const
{
    const QVariant variant = d->m_data.value(scFtpProxy);
    if (variant.canConvert<QNetworkProxy>())
        return variant.value<QNetworkProxy>();
    return QNetworkProxy();
}

void Settings::setFtpProxy(const QNetworkProxy &proxy)
{
    d->m_data.insert(scFtpProxy, QVariant::fromValue(proxy));
}

QNetworkProxy Settings::httpProxy() const
{
    const QVariant variant = d->m_data.value(scHttpProxy);
    if (variant.canConvert<QNetworkProxy>())
        return variant.value<QNetworkProxy>();
    return QNetworkProxy();
}

void Settings::setHttpProxy(const QNetworkProxy &proxy)
{
    d->m_data.insert(scHttpProxy, QVariant::fromValue(proxy));
}

QStringList Settings::translations() const
{
    const QVariant variant = d->m_data.values(scTranslations);
    if (variant.canConvert<QStringList>())
        return variant.value<QStringList>();
    return QStringList();
}

void Settings::setTranslations(const QStringList &translations)
{
    d->m_data.insert(scTranslations, translations);
}

QString Settings::controlScript() const
{
    return d->m_data.value(QLatin1String(scControlScript)).toString();
}

bool Settings::supportsModify() const
{
    return d->m_data.value(scSupportsModify, true).toBool();
}
