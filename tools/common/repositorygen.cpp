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
#include "repositorygen.h"

#include <fileio.h>
#include <fileutils.h>
#include <errors.h>
#include <globals.h>
#include <lib7z_create.h>
#include <lib7z_facade.h>
#include <lib7z_list.h>
#include <settings.h>
#include <qinstallerglobal.h>
#include <utils.h>
#include <scriptengine.h>

#include <updater.h>

#include <QtCore/QDirIterator>
#include <QtCore/QRegExp>
#include <QtCore/QXmlStreamReader>

#include <QtXml/QDomDocument>

#include <iostream>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

using namespace QInstallerTools;

namespace {

class ArchiveLink
{
public:
    explicit ArchiveLink(const QString &path)
        : m_path(path)
    {
        load();
    }

    bool isValid() const
    {
        return !m_target.isEmpty() && !m_target.isEmpty() && m_uncompressedSize != 0 &&
            m_compressedSize != 0;
    }

    operator bool() const { return isValid(); }

    QString path() const { return m_path; }
    QString target() const { return m_target; }
    QString sha1() const { return m_sha1; }
    qint64 uncompressedSize() const { return m_uncompressedSize; }
    qint64 compressedSize() const { return m_compressedSize; }

private:
    // Try to load the file at m_path. If the file is obviously not an ArchiveLink file, no error is
    // reported beside that subsequent call to isValid() returns false. In other cases exception is
    // thrown on errors.
    void load()
    {
        QFileInfo info(m_path);
        if (info.suffix() != QLatin1String("link"))
            return;

        QFile file(m_path);
        if (!file.open(QIODevice::ReadOnly)) {
            throw QInstaller::Error(QString::fromLatin1("Error opening file \"%1\" for reading: %2")
                    .arg(m_path)
                    .arg(file.errorString()));
        }

        QXmlStreamReader xml(&file);
        if (xml.readNextStartElement() && xml.name() == QLatin1String("ArchiveLink")
                && xml.attributes().value(QLatin1String("Version")) == QLatin1String("1.0")) {
            while (xml.readNextStartElement()) {
                if (xml.name() == QLatin1String("Target"))
                    m_target = xml.readElementText();
                else if (xml.name() == QLatin1String("SHA1"))
                    m_sha1 = xml.readElementText();
                else if (xml.name() == QLatin1String("UncompressedSize"))
                    m_uncompressedSize = xml.readElementText().toInt();
                else if (xml.name() == QLatin1String("CompressedSize"))
                    m_compressedSize = xml.readElementText().toInt();
                else
                    xml.skipCurrentElement();
            }
        } else {
            qWarning() << "Despite of the file extension this does not seem to be an ArchiveLink version 1.0 file:" << m_path;
            return;
        }
        if (xml.hasError()) {
            throw QInstaller::Error(QString::fromLatin1("Error parsing file \"%1\": %2")
                    .arg(m_path)
                    .arg(xml.errorString()));
        }
        if (!isValid())
            throw QInstaller::Error(QString::fromLatin1("Incomplete or otherwise broken file \"%1\"").arg(m_path));
    }

private:
    QString m_path;
    QString m_target;
    QString m_sha1;
    qint64 m_uncompressedSize = 0;
    qint64 m_compressedSize = 0;
};

} // namespace anonymous

void QInstallerTools::printRepositoryGenOptions()
{
    std::cout << "  -p|--packages dir         The directory containing the available packages." << std::endl;
    std::cout << "                            This entry can be given multiple times." << std::endl;

    std::cout << "  -e|--exclude p1,...,pn    Exclude the given packages." << std::endl;
    std::cout << "  -i|--include p1,...,pn    Include the given packages and their dependencies" << std::endl;
    std::cout << "                            from the repository." << std::endl;

    std::cout << "  --ignore-translations     Do not use any translation" << std::endl;
    std::cout << "  --ignore-invalid-packages Ignore all invalid packages instead of aborting." << std::endl;
}

QString QInstallerTools::makePathAbsolute(const QString &path)
{
    if (QFileInfo(path).isRelative())
        return QDir::current().absoluteFilePath(path);
    return path;
}

void QInstallerTools::copyWithException(const QString &source, const QString &target, const QString &kind)
{
    qDebug() << "Copying associated" << kind << "file" << source;

    const QFileInfo targetFileInfo(target);
    if (!targetFileInfo.dir().exists())
        QInstaller::mkpath(targetFileInfo.absolutePath());

    QFile sourceFile(source);
    if (!sourceFile.copy(target)) {
        qDebug() << "failed!\n";
        throw QInstaller::Error(QString::fromLatin1("Cannot copy the %1 file from \"%2\" to \"%3\": "
            "%4").arg(kind, QDir::toNativeSeparators(source), QDir::toNativeSeparators(target),
            /* in case of an existing target the error String does not show the file */
            (targetFileInfo.exists() ? QLatin1String("Target already exist.") : sourceFile.errorString())));
    }

    qDebug() << "done.\n";
}

static QStringList copyFilesFromNode(const QString &parentNode, const QString &childNode, const QString &attr,
    const QString &kind, const QDomNode &package, const PackageInfo &info, const QString &targetDir)
{
    QStringList copiedFiles;
    const QDomNodeList nodes = package.firstChildElement(parentNode).childNodes();
    for (int i = 0; i < nodes.count(); ++i) {
        const QDomNode node = nodes.at(i);
        if (node.nodeName() != childNode)
            continue;

        const QDir dir(QString::fromLatin1("%1/meta").arg(info.directory));
        const QString filter = attr.isEmpty() ? node.toElement().text() : node.toElement().attribute(attr);
        const QStringList files = dir.entryList(QStringList(filter), QDir::Files);
        if (files.isEmpty()) {
            throw QInstaller::Error(QString::fromLatin1("Cannot find any %1 matching \"%2\" "
                "while copying %1 of \"%3\".").arg(kind, filter, info.name));
        }

        foreach (const QString &file, files) {
            const QString source(QString::fromLatin1("%1/meta/%2").arg(info.directory, file));
            const QString target(QString::fromLatin1("%1/%2/%3").arg(targetDir, info.name, file));
            copyWithException(source, target, kind);
            copiedFiles.append(file);
        }
    }
    return copiedFiles;
}

void QInstallerTools::copyMetaData(const QString &_targetDir, const QString &metaDataDir,
    const PackageInfoVector &packages, const QString &appName, const QString &appVersion)
{
    const QString targetDir = makePathAbsolute(_targetDir);
    if (!QFile::exists(targetDir))
        QInstaller::mkpath(targetDir);

    QDomDocument doc;
    QDomElement root;
    QFile existingUpdatesXml(QFileInfo(metaDataDir, QLatin1String("Updates.xml")).absoluteFilePath());
    if (existingUpdatesXml.open(QIODevice::ReadOnly) && doc.setContent(&existingUpdatesXml)) {
        root = doc.documentElement();
        // remove entry for this component from existing Updates.xml, if found
        foreach (const PackageInfo &info, packages) {
            const QDomNodeList packageNodes = root.childNodes();
            for (int i = packageNodes.count() - 1; i >= 0; --i) {
                const QDomNode node = packageNodes.at(i);
                if (node.nodeName() != QLatin1String("PackageUpdate"))
                    continue;
                if (node.firstChildElement(QLatin1String("Name")).text() != info.name)
                    continue;
                root.removeChild(node);
            }
        }
        existingUpdatesXml.close();
    } else {
        root = doc.createElement(QLatin1String("Updates"));
        root.appendChild(doc.createElement(QLatin1String("ApplicationName"))).appendChild(doc
            .createTextNode(appName));
        root.appendChild(doc.createElement(QLatin1String("ApplicationVersion"))).appendChild(doc
            .createTextNode(appVersion));
        root.appendChild(doc.createElement(QLatin1String("Checksum"))).appendChild(doc
            .createTextNode(QLatin1String("true")));
    }

    foreach (const PackageInfo &info, packages) {
        if (!QDir(targetDir).mkpath(info.name))
            throw QInstaller::Error(QString::fromLatin1("Cannot create directory \"%1\".").arg(info.name));

        const QString packageXmlPath = QString::fromLatin1("%1/meta/package.xml").arg(info.directory);
        qDebug() << "Copy meta data for package" << info.name << "using" << packageXmlPath;

        QFile file(packageXmlPath);
        QInstaller::openForRead(&file);

        QString errMsg;
        int line = 0;
        int column = 0;
        QDomDocument packageXml;
        if (!packageXml.setContent(&file, &errMsg, &line, &column)) {
            throw QInstaller::Error(QString::fromLatin1("Cannot parse \"%1\": line: %2, column: %3: %4 (%5)")
                .arg(QDir::toNativeSeparators(packageXmlPath)).arg(line).arg(column).arg(errMsg, info.name));
        }

        QDomElement update = doc.createElement(QLatin1String("PackageUpdate"));
        QDomNode nameElement = update.appendChild(doc.createElement(QLatin1String("Name")));
        nameElement.appendChild(doc.createTextNode(info.name));

        // list of current unused or later transformed tags
        QStringList blackList;
        blackList << QLatin1String("UserInterfaces") << QLatin1String("Translations") <<
            QLatin1String("Licenses") << QLatin1String("Name");

        bool foundDefault = false;
        bool foundVirtual = false;
        bool foundDisplayName = false;
        bool foundDownloadableArchives = false;
        bool foundCheckable = false;
        const QDomNode package = packageXml.firstChildElement(QLatin1String("Package"));
        const QDomNodeList childNodes = package.childNodes();
        for (int i = 0; i < childNodes.count(); ++i) {
            const QDomNode node = childNodes.at(i);
            const QString key = node.nodeName();

            if (key == QLatin1String("Default"))
                foundDefault = true;
            if (key == QLatin1String("Virtual"))
                foundVirtual = true;
            if (key == QLatin1String("DisplayName"))
                foundDisplayName = true;
            if (key == QLatin1String("DownloadableArchives"))
                foundDownloadableArchives = true;
            if (key == QLatin1String("Checkable"))
                foundCheckable = true;
            if (node.isComment() || blackList.contains(key))
                continue;   // just skip comments and some tags...

            QDomElement element = doc.createElement(key);
            for (int j = 0; j < node.attributes().size(); ++j) {
                element.setAttribute(node.attributes().item(j).toAttr().name(),
                    node.attributes().item(j).toAttr().value());
            }
            update.appendChild(element).appendChild(doc.createTextNode(node.toElement().text()));
        }

        if (foundDefault && foundVirtual) {
            throw QInstaller::Error(QString::fromLatin1("Error: <Default> and <Virtual> elements are "
                "mutually exclusive in file \"%1\".").arg(QDir::toNativeSeparators(packageXmlPath)));
        }

        if (foundDefault && foundCheckable) {
            throw QInstaller::Error(QString::fromLatin1("Error: <Default> and <Checkable>"
                "elements are mutually exclusive in file \"%1\".")
                .arg(QDir::toNativeSeparators(packageXmlPath)));
        }

        if (!foundDisplayName) {
            qWarning() << "No DisplayName tag found at" << info.name << ", using component Name instead.";
            QDomElement displayNameElement = doc.createElement(QLatin1String("DisplayName"));
            update.appendChild(displayNameElement).appendChild(doc.createTextNode(info.name));
        }

        // get the size of the data
        quint64 componentSize = 0;
        quint64 compressedComponentSize = 0;

        const QDir::Filters filters = QDir::Files | QDir::NoDotAndDotDot;
        const QDir dataDir = QString::fromLatin1("%1/%2/data").arg(metaDataDir, info.name);
        const QFileInfoList entries = dataDir.exists() ? dataDir.entryInfoList(filters | QDir::Dirs)
            : QDir(QString::fromLatin1("%1/%2").arg(metaDataDir, info.name)).entryInfoList(filters);
        qDebug() << "calculate size of directory" << dataDir.absolutePath();
        foreach (const QFileInfo &fi, entries) {
            try {
                if (fi.isDir()) {
                    QDirIterator recursDirIt(fi.filePath(), QDirIterator::Subdirectories);
                    while (recursDirIt.hasNext()) {
                        recursDirIt.next();
                        const quint64 size = QInstaller::fileSize(recursDirIt.fileInfo());
                        componentSize += size;
                        compressedComponentSize += size;
                    }
                } else if (fi.isSymLink()) {
                    // noop. The only way a symlink can appear here is through ArchiveLink in which
                    // case the size is added below
                } else if (Lib7z::isSupportedArchive(fi.filePath())) {
                    // if it's an archive already, list its files and sum the uncompressed sizes
                    QFile archive(fi.filePath());
                    compressedComponentSize += archive.size();
                    QInstaller::openForRead(&archive);

                    QVector<Lib7z::File>::const_iterator fileIt;
                    const QVector<Lib7z::File> files = Lib7z::listArchive(&archive);
                    for (fileIt = files.begin(); fileIt != files.end(); ++fileIt)
                        componentSize += fileIt->uncompressedSize;
                } else {
                    // otherwise just add its size
                    const quint64 size = QInstaller::fileSize(fi);
                    componentSize += size;
                    compressedComponentSize += size;
                }
            } catch (const QInstaller::Error &error) {
                qDebug().noquote() << error.message();
            } catch(...) {
                // ignore, that's just about the sizes - and size doesn't matter, you know?
            }
        }

        // add size declared through ArchiveLink
        componentSize += info.linkedFilesUncompressedSize;
        compressedComponentSize += info.linkedFilesCompressedSize;

        QDomElement fileElement = doc.createElement(QLatin1String("UpdateFile"));
        fileElement.setAttribute(QLatin1String("UncompressedSize"), componentSize);
        fileElement.setAttribute(QLatin1String("CompressedSize"), compressedComponentSize);
        // adding the OS attribute to be compatible with old sdks
        fileElement.setAttribute(QLatin1String("OS"), QLatin1String("Any"));
        update.appendChild(fileElement);

        root.appendChild(update);

        // copy script file
        const QString script = package.firstChildElement(QLatin1String("Script")).text();
        if (!script.isEmpty()) {
            QFile scriptFile(QString::fromLatin1("%1/meta/%2").arg(info.directory, script));
            if (!scriptFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
                throw QInstaller::Error(QString::fromLatin1("Cannot open component script at \"%1\".")
                    .arg(QDir::toNativeSeparators(scriptFile.fileName())));
            }

            const QString scriptContent = QLatin1String("(function() {")
                + QString::fromUtf8(scriptFile.readAll())
                + QLatin1String(";"
                "    if (typeof Component == \"undefined\")"
                "        throw \"Missing Component constructor. Please check your script.\";"
                "})();");

            // if the user isn't aware of the downloadable archives value we will add it automatically later
            foundDownloadableArchives |= scriptContent.contains(QLatin1String("addDownloadableArchive"))
                || scriptContent.contains(QLatin1String("removeDownloadableArchive"));

            static QInstaller::ScriptEngine testScriptEngine;
            const QJSValue value = testScriptEngine.evaluate(scriptContent, scriptFile.fileName());
            if (value.isError()) {
                throw QInstaller::Error(QString::fromLatin1("Exception while loading component "
                    "script at \"%1\": %2").arg(QDir::toNativeSeparators(scriptFile.fileName()),
                            value.toString().isEmpty() ? QString::fromLatin1("Unknown error.") :
                            value.toString() + QStringLiteral(" on line number: ") +
                                value.property(QStringLiteral("lineNumber")).toString()));
            }

            const QString toLocation(QString::fromLatin1("%1/%2/%3").arg(targetDir, info.name, script));
            copyWithException(scriptFile.fileName(), toLocation, QInstaller::scScript);
        }

        // write DownloadableArchives tag if that is missed by the user
        if (!foundDownloadableArchives && !info.copiedFiles.isEmpty()) {
            QStringList realContentFiles;
            foreach (const QString &filePath, info.copiedFiles) {
                if (!filePath.endsWith(QLatin1String(".sha1"), Qt::CaseInsensitive)) {
                    const QString fileName = QFileInfo(filePath).fileName();
                    // remove unnecessary version string from filename and add it to the list
                    realContentFiles.append(fileName.mid(info.version.count()));
                }
            }

            update.appendChild(doc.createElement(QLatin1String("DownloadableArchives"))).appendChild(doc
                .createTextNode(realContentFiles.join(QChar::fromLatin1(','))));
        }

        // copy user interfaces
        const QStringList uiFiles = copyFilesFromNode(QLatin1String("UserInterfaces"),
            QLatin1String("UserInterface"), QString(), QLatin1String("user interface"), package, info,
            targetDir);
        if (!uiFiles.isEmpty()) {
            update.appendChild(doc.createElement(QLatin1String("UserInterfaces"))).appendChild(doc
                .createTextNode(uiFiles.join(QChar::fromLatin1(','))));
        }

        // copy translations
        QStringList trFiles;
        if (!qApp->arguments().contains(QString::fromLatin1("--ignore-translations"))) {
            trFiles = copyFilesFromNode(QLatin1String("Translations"), QLatin1String("Translation"),
                QString(), QLatin1String("translation"), package, info, targetDir);
            if (!trFiles.isEmpty()) {
                update.appendChild(doc.createElement(QLatin1String("Translations"))).appendChild(doc
                    .createTextNode(trFiles.join(QChar::fromLatin1(','))));
            }
        }

        // copy license files
        const QStringList licenses = copyFilesFromNode(QLatin1String("Licenses"), QLatin1String("License"),
            QLatin1String("file"), QLatin1String("license"), package, info, targetDir);
        if (!licenses.isEmpty()) {
            foreach (const QString &trFile, trFiles) {
                // Copy translated license file based on the assumption that it will have the same base name
                // as the original license plus the file name of an existing translation file without suffix.
                foreach (const QString &license, licenses) {
                    const QFileInfo untranslated(license);
                    const QString translatedLicense = QString::fromLatin1("%2_%3.%4").arg(untranslated
                        .baseName(), QFileInfo(trFile).baseName(), untranslated.completeSuffix());
                    // ignore copy failure, that's just about the translations
                    QFile::copy(QString::fromLatin1("%1/meta/%2").arg(info.directory).arg(translatedLicense),
                        QString::fromLatin1("%1/%2/%3").arg(targetDir, info.name, translatedLicense));
                }
            }
            update.appendChild(package.firstChildElement(QLatin1String("Licenses")).cloneNode());
        }
    }
    doc.appendChild(root);

    QFile targetUpdatesXml(targetDir + QLatin1String("/Updates.xml"));
    QInstaller::openForWrite(&targetUpdatesXml);
    QInstaller::blockingWrite(&targetUpdatesXml, doc.toByteArray());
}

PackageInfoVector QInstallerTools::createListOfPackages(const QStringList &packagesDirectories,
    QStringList *packagesToFilter, FilterType filterType)
{
    qDebug() << "\nCollecting information about available packages...";

    bool ignoreInvalidPackages = qApp->arguments().contains(QString::fromLatin1("--ignore-invalid-packages"));

    PackageInfoVector dict;
    QFileInfoList entries;
    foreach (const QString &packagesDirectory, packagesDirectories)
        entries.append(QDir(packagesDirectory).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot));
    for (QFileInfoList::const_iterator it = entries.constBegin(); it != entries.constEnd(); ++it) {
        if (filterType == Exclude) {
            // Check for current file in exclude list, if found, skip it and remove it from exclude list
            if (packagesToFilter->contains(it->fileName())) {
                packagesToFilter->removeAll(it->fileName());
                continue;
            }
        } else {
            // Check for current file in include list, if not found, skip it; if found, remove it from include list
            if (!packagesToFilter->contains(it->fileName()))
                continue;
            packagesToFilter->removeAll(it->fileName());
        }
        qDebug() << "Found subdirectory" << it->fileName();
        // because the filter is QDir::Dirs - filename means the name of the subdirectory
        if (it->fileName().contains(QLatin1Char('-'))) {
            if (ignoreInvalidPackages)
                continue;
            throw QInstaller::Error(QString::fromLatin1("Component \"%1\" must not contain '-'. This is not "
                "allowed, because dashes are used as the separator between the component name and the "
                "version number internally.").arg(QDir::toNativeSeparators(it->fileName())));
        }

        QFile file(QString::fromLatin1("%1/meta/package.xml").arg(it->filePath()));
        QFileInfo fileInfo(file);
        if (!fileInfo.exists()) {
            if (ignoreInvalidPackages)
                continue;
            throw QInstaller::Error(QString::fromLatin1("Component \"%1\" does not contain a package "
                "description (meta/package.xml is missing).").arg(QDir::toNativeSeparators(it->fileName())));
        }

        file.open(QIODevice::ReadOnly);

        QDomDocument doc;
        QString error;
        int errorLine = 0;
        int errorColumn = 0;
        if (!doc.setContent(&file, &error, &errorLine, &errorColumn)) {
            if (ignoreInvalidPackages)
                continue;
            throw QInstaller::Error(QString::fromLatin1("Component package description in \"%1\" is invalid. "
                "Error at line: %2, column: %3 -> %4").arg(QDir::toNativeSeparators(fileInfo.absoluteFilePath()),
                                                           QString::number(errorLine),
                                                           QString::number(errorColumn), error));
        }

        const QDomElement packageElement = doc.firstChildElement(QLatin1String("Package"));
        const QString name = packageElement.firstChildElement(QLatin1String("Name")).text();
        if (!name.isEmpty() && name != it->fileName()) {
            qWarning().nospace() << "The <Name> tag in the file " << fileInfo.absoluteFilePath()
                       << " is ignored - the installer uses the path element right before the 'meta'"
                       << " (" << it->fileName() << ")";
        }

        QString releaseDate = packageElement.firstChildElement(QLatin1String("ReleaseDate")).text();
        if (releaseDate.isEmpty()) {
            qWarning("Release date for \"%s\" is empty! Using the current date instead.",
                qPrintable(fileInfo.absoluteFilePath()));
            releaseDate = QDate::currentDate().toString(Qt::ISODate);
        }

        if (!QDate::fromString(releaseDate, Qt::ISODate).isValid()) {
            if (ignoreInvalidPackages)
                continue;
            throw QInstaller::Error(QString::fromLatin1("Release date for \"%1\" is invalid! <ReleaseDate>%2"
                "</ReleaseDate>. Supported format: YYYY-MM-DD").arg(QDir::toNativeSeparators(fileInfo.absoluteFilePath()),
                                                                    releaseDate));
        }

        PackageInfo info;
        info.name = it->fileName();
        info.version = packageElement.firstChildElement(QLatin1String("Version")).text();
        if (!QRegExp(QLatin1String("[0-9]+((\\.|-)[0-9]+)*")).exactMatch(info.version)) {
            if (ignoreInvalidPackages)
                continue;
            throw QInstaller::Error(QString::fromLatin1("Component version for \"%1\" is invalid! <Version>%2</Version>")
                .arg(QDir::toNativeSeparators(fileInfo.absoluteFilePath()), info.version));
        }
        info.dependencies = packageElement.firstChildElement(QLatin1String("Dependencies")).text()
            .split(QInstaller::commaRegExp(), QString::SkipEmptyParts);
        info.directory = it->filePath();
        dict.push_back(info);

        qDebug() << "- it provides the package" << info.name << " - " << info.version;
    }

    if (!packagesToFilter->isEmpty() && packagesToFilter->at(0) != QString::fromLatin1(
        "X_fake_filter_component_for_online_only_installer_X")) {
        qWarning() << "The following explicitly given packages could not be found\n in package directory:" << *packagesToFilter;
    }

    if (dict.isEmpty())
        qDebug() << "No available packages found at the specified location.";

    return dict;
}

QHash<QString, QString> QInstallerTools::buildPathToVersionMapping(const PackageInfoVector &info)
{
    QHash<QString, QString> map;
    foreach (const PackageInfo &inf, info)
        map[inf.name] = inf.version;
    return map;
}

static void writeSHA1ToNodeWithName(QDomDocument &doc, QDomNodeList &list, const QByteArray &sha1sum,
    const QString &nodename)
{
    qDebug() << "searching sha1sum node for" << nodename;
    for (int i = 0; i < list.size(); ++i) {
        QDomNode curNode = list.at(i);
        QDomNode nameTag = curNode.firstChildElement(QLatin1String("Name"));
        if (!nameTag.isNull() && nameTag.toElement().text() == nodename) {
            QDomNode sha1Node = doc.createElement(QLatin1String("SHA1"));
            sha1Node.appendChild(doc.createTextNode(QString::fromLatin1(sha1sum.toHex().constData())));
            curNode.appendChild(sha1Node);
        }
    }
}

void QInstallerTools::compressMetaDirectories(const QString &repoDir, const QString &baseDir,
    const QHash<QString, QString> &versionMapping)
{
    QDomDocument doc;
    QDomElement root;
    // use existing Updates.xml, if any
    QFile existingUpdatesXml(QFileInfo(QDir(repoDir), QLatin1String("Updates.xml")).absoluteFilePath());
    if (!existingUpdatesXml.open(QIODevice::ReadOnly) || !doc.setContent(&existingUpdatesXml)) {
        qDebug() << "Cannot find Updates.xml";
    } else {
        root = doc.documentElement();
    }
    existingUpdatesXml.close();

    QDir dir(repoDir);
    const QStringList sub = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    QDomNodeList elements =  doc.elementsByTagName(QLatin1String("PackageUpdate"));
    foreach (const QString &i, sub) {
        QDir sd(dir);
        sd.cd(i);
        const QString path = QString(i).remove(baseDir);
        const QString versionPrefix = versionMapping[path];
        if (path.isNull())
            continue;
        const QString absPath = sd.absolutePath();
        const QString fn = QLatin1String(versionPrefix.toLatin1() + "meta.7z");
        const QString tmpTarget = repoDir + QLatin1String("/") +fn;
        Lib7z::createArchive(tmpTarget, QStringList() << absPath, Lib7z::QTmpFile::No);

        // remove the files that got compressed
        QInstaller::removeFiles(absPath, true);

        QFile tmp(tmpTarget);
        tmp.open(QFile::ReadOnly);
        const QByteArray sha1Sum = QInstaller::calculateHash(&tmp, QCryptographicHash::Sha1);
        writeSHA1ToNodeWithName(doc, elements, sha1Sum, path);
        const QString finalTarget = absPath + QLatin1String("/") + fn;
        if (!tmp.rename(finalTarget)) {
            throw QInstaller::Error(QString::fromLatin1("Cannot move file \"%1\" to \"%2\".").arg(
                                        QDir::toNativeSeparators(tmpTarget), QDir::toNativeSeparators(finalTarget)));
        }
    }

    QInstaller::openForWrite(&existingUpdatesXml);
    QInstaller::blockingWrite(&existingUpdatesXml, doc.toByteArray());
    existingUpdatesXml.close();
}

void QInstallerTools::copyComponentData(const QStringList &packageDirs, const QString &repoDir,
    PackageInfoVector *const infos)
{
    for (int i = 0; i < infos->count(); ++i) {
        const PackageInfo info = infos->at(i);
        const QString name = info.name;
        qDebug() << "Copying component data for" << name;

        const QString namedRepoDir = QString::fromLatin1("%1/%2").arg(repoDir, name);
        if (!QDir().mkpath(namedRepoDir)) {
            throw QInstaller::Error(QString::fromLatin1("Cannot create repository directory for component \"%1\".")
                .arg(name));
        }

        QStringList compressedFiles;
        QStringList filesToCompress;
        foreach (const QString &packageDir, packageDirs) {
            const QDir dataDir(QString::fromLatin1("%1/%2/data").arg(packageDir, name));
            foreach (const QString &entry, dataDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Files)) {
                QFileInfo fileInfo(dataDir.absoluteFilePath(entry));
                if (fileInfo.isFile() && !fileInfo.isSymLink()) {
                    const QString absoluteEntryFilePath = dataDir.absoluteFilePath(entry);
                    if (Lib7z::isSupportedArchive(absoluteEntryFilePath)) {
                        QFile tmp(absoluteEntryFilePath);
                        QString target = QString::fromLatin1("%1/%3%2").arg(namedRepoDir, entry, info.version);
                        qDebug() << "Copying archive from" << tmp.fileName() << "to" << target;
                        if (!tmp.copy(target)) {
                            throw QInstaller::Error(QString::fromLatin1("Cannot copy file \"%1\" to \"%2\": %3")
                                .arg(QDir::toNativeSeparators(tmp.fileName()), QDir::toNativeSeparators(target), tmp.errorString()));
                        }
                        compressedFiles.append(target);
                    } else if (ArchiveLink link = ArchiveLink(absoluteEntryFilePath)) {
                        QString target = QString::fromLatin1("%1/%3%2").arg(namedRepoDir, fileInfo.completeBaseName(), info.version);
                        qDebug() << "Creating archive link" << target << "to" << link.target() << "sha1 hash" << link.sha1();
#ifdef Q_OS_WIN
                        throw QInstaller::Error(QString::fromLatin1("Archive links not supported on Windows"));
#else
                        int result;
                        result = symlink(link.target().toUtf8(), target.toUtf8());
                        if (result != 0) {
                            throw QInstaller::Error(QString::fromLatin1("Cannot create symbolic link \"%1\" to \"%2\" (error code %3)")
                                    .arg(QDir::toNativeSeparators(target), QDir::toNativeSeparators(link.target()),
                                        QString::number(result)));
                        }
#endif
                        (*infos)[i].copiedFiles.append(target);
                        (*infos)[i].linkedFilesUncompressedSize += link.uncompressedSize();
                        (*infos)[i].linkedFilesCompressedSize += link.compressedSize();
                        QFile hashFile(target + QLatin1String(".sha1"));
                        QInstaller::openForWrite(&hashFile);
                        hashFile.write(link.sha1().toUtf8());
                        (*infos)[i].copiedFiles.append(hashFile.fileName());
                        hashFile.close();
                    } else {
                        filesToCompress.append(absoluteEntryFilePath);
                    }
                } else if (fileInfo.isDir()) {
                    qDebug() << "Compressing data directory" << entry;
                    QString target = QString::fromLatin1("%1/%3%2.7z").arg(namedRepoDir, entry, info.version);
                    Lib7z::createArchive(target, QStringList() << dataDir.absoluteFilePath(entry),
                        Lib7z::QTmpFile::No);
                    compressedFiles.append(target);
                } else if (fileInfo.isSymLink()) {
                    filesToCompress.append(dataDir.absoluteFilePath(entry));
                }
            }
        }

        if (!filesToCompress.isEmpty()) {
            qDebug() << "Compressing files found in data directory:" << filesToCompress;
            QString target = QString::fromLatin1("%1/%3%2").arg(namedRepoDir, QLatin1String("content.7z"),
                info.version);
            Lib7z::createArchive(target, filesToCompress, Lib7z::QTmpFile::No);
            compressedFiles.append(target);
        }

        foreach (const QString &target, compressedFiles) {
            (*infos)[i].copiedFiles.append(target);

            QFile archiveFile(target);
            QFile archiveHashFile(archiveFile.fileName() + QLatin1String(".sha1"));

            qDebug() << "Hash is stored in" << archiveHashFile.fileName();
            qDebug() << "Creating hash of archive" << archiveFile.fileName();

            try {
                QInstaller::openForRead(&archiveFile);
                const QByteArray hashOfArchiveData = QInstaller::calculateHash(&archiveFile,
                    QCryptographicHash::Sha1).toHex();
                archiveFile.close();

                QInstaller::openForWrite(&archiveHashFile);
                archiveHashFile.write(hashOfArchiveData);
                qDebug() << "Generated sha1 hash:" << hashOfArchiveData;
                (*infos)[i].copiedFiles.append(archiveHashFile.fileName());
                archiveHashFile.close();
            } catch (const QInstaller::Error &/*e*/) {
                archiveFile.close();
                archiveHashFile.close();
                throw;
            }
        }
    }
}
