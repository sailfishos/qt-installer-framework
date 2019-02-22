/**************************************************************************
**
** Copyright (C) 2019 Jolla Ltd.
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

#include "vmshutdownoperation.h"

#include "qprocesswrapper.h"

#include <QtCore/QDir>
#include <QtCore/QRegularExpression>
#include <QtCore/QSettings>
#include <QtCore/QThread>

#include <memory>

using namespace QInstaller;

const char VBOXMANAGE[] = "VBoxManage";

class VmShutdownOperation::Private
{
public:
    explicit Private(VmShutdownOperation* qq)
        : q(qq)
    {
    }

private:
    QString vBoxManagePath();
    bool executeVboxCommand(const QStringList& args, QString* output = nullptr);
    bool isVmRunning(const QString& vmName);

    VmShutdownOperation* const q;

public:
    bool run(const QString &vmName);
};

VmShutdownOperation::VmShutdownOperation(PackageManagerCore* core)
    : Operation(core)
    , d(new Private(this))
{
    setName(QLatin1String("VmShutdown"));
}

VmShutdownOperation::~VmShutdownOperation()
{
    delete d;
}

void VmShutdownOperation::backup()
{
}

bool VmShutdownOperation::performOperation()
{
    // This operation needs at least one argument.
    // It is the name of the vm to close.
    if (!checkArgumentCount(1, INT_MAX))
        return false;

    QString vmName;
    bool runNow = true;
    foreach (const QString &argument, arguments()) {
        if (argument == QLatin1String("when=undo")) {
            runNow = false;
        } else {
            if (!vmName.isEmpty()) {
                setErrorString(tr("VmShutdown only accepts one argument (vm name)"));
                return false;
            }
            vmName = argument;
        }
    }

    return runNow ? d->run(vmName) : true;
}

bool VmShutdownOperation::undoOperation()
{
    QString vmName;
    bool runNow = false;
    foreach (const QString &argument, arguments()) {
        if (argument == QLatin1String("when=undo")) {
            runNow = true;
        } else {
            if (!vmName.isEmpty()) {
                setErrorString(tr("VmShutdown only accepts one argument (vm name)"));
                return false;
            }
            vmName = argument;
        }
    }
    return runNow ? d->run(vmName) : true;
}

bool VmShutdownOperation::testOperation()
{
    return true;
}

void VmShutdownOperation::cancelOperation()
{
    emit cancelProcess();
}

QString VmShutdownOperation::Private::vBoxManagePath()
{
    static QString path;
    if (!path.isNull())
        return path;

#if defined(Q_OS_WIN)
    path = QString::fromLocal8Bit(qgetenv("VBOX_INSTALL_PATH"));
    if (path.isEmpty()) {
        // env var name for VirtualBox 4.3.12 changed to this
        path = QString::fromLocal8Bit(qgetenv("VBOX_MSI_INSTALL_PATH"));
        if (path.isEmpty()) {
            // Not found in environment? Look up registry.
            QSettings s(QLatin1String("HKEY_LOCAL_MACHINE\\SOFTWARE\\Oracle\\VirtualBox"),
                        QSettings::NativeFormat);
            path = s.value(QLatin1String("InstallDir")).toString();
            if (path.startsWith(QLatin1Char('"')) && path.endsWith(QLatin1Char('"')))
                path = path.mid(1, path.length() - 2); // remove quotes
        }
    }

    if (!path.isEmpty())
        path.append(QDir::separator() + QLatin1String(VBOXMANAGE));
#else
    QStringList searchPaths = QProcessEnvironment::systemEnvironment()
        .value(QLatin1String("PATH")).split(QLatin1Char(':'));
    // VBox 5 installs here for compatibility with Mac OS X 10.11
    searchPaths.append(QLatin1String("/usr/local/bin"));

    foreach (const QString &searchPath, searchPaths) {
        QDir dir(searchPath);
        if (dir.exists(QLatin1String(VBOXMANAGE))) {
            path = dir.absoluteFilePath(QLatin1String(VBOXMANAGE));
            break;
        }
    }
#endif // Q_OS_WIN

    return path;
}

bool VmShutdownOperation::Private::executeVboxCommand(const QStringList &args, QString *output)
{
    std::unique_ptr<QProcessWrapper> process(new QProcessWrapper());

    connect(q, &VmShutdownOperation::cancelProcess, process.get(), &QProcessWrapper::cancel);

    //we still like the none blocking possibility to perform this operation without threads
    QEventLoop loop;
    if (QThread::currentThread() == qApp->thread())
        QObject::connect(process.get(), &QProcessWrapper::finished, &loop, &QEventLoop::quit);

    if (output) {
        QObject::connect(process.get(), &QProcessWrapper::readyRead,
                         [&]() {
                            QByteArray processOutput = process->readAll();
                            output->append(QString::fromLocal8Bit(processOutput));
                         });
    }

    process->start(vBoxManagePath(), args);

    bool success = false;
    //we still like the none blocking possibility to perform this operation without threads
    if (QThread::currentThread() == qApp->thread())
        success = process->waitForStarted();
    else
        success = process->waitForFinished(-1);

    if (!success) {
        q->setError(UserDefinedError);
        q->setErrorString(tr("Cannot start VBoxManage: %2").arg(process->errorString()));
        return false;
    }

    if (QThread::currentThread() == qApp->thread()) {
        if (process->state() != QProcessWrapper::NotRunning)
            loop.exec();
    }

    if (process->exitStatus() != QProcessWrapper::NormalExit) {
        q->setError(UserDefinedError);
        q->setErrorString(tr("VBoxManage crashed!"));
        return false;
    }

    return true;
}

bool VmShutdownOperation::Private::isVmRunning(const QString &vmName)
{
    QStringList args;
    args << QLatin1String("showvminfo") << vmName;
    QString output;
    if (!executeVboxCommand(args, &output))
        return true;

    QRegularExpression re(QStringLiteral("^Session (name|type):"), QRegularExpression::MultilineOption);
    return re.match(output).hasMatch();
}

bool VmShutdownOperation::Private::run(const QString& vmName)
{
    if (vmName.isEmpty())
        return false;

    QStringList shutdownArgs;
    shutdownArgs << QLatin1String("controlvm") << vmName << QLatin1String("poweroff");

    if (!executeVboxCommand(shutdownArgs))
        return false;

    QTime timeout;
    timeout.start();

    while (timeout.elapsed() < 30000 ) {
        if (!isVmRunning(vmName))
            return true;
    }
    if (q->error() == NoError) {
        q->setError(UserDefinedError);
        q->setErrorString(tr("Virtual Machine %1 is still running").arg(vmName));
    }
    return false;
}

