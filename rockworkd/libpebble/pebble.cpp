#include "pebble.h"
#include "watchconnection.h"
#include "notificationendpoint.h"
#include "watchdatareader.h"
#include "watchdatawriter.h"
#include "musicendpoint.h"
#include "phonecallendpoint.h"
#include "appmanager.h"
#include "appmsgmanager.h"
#include "jskit/jskitmanager.h"
#include "blobdb.h"
#include "appdownloader.h"
#include "screenshotendpoint.h"
#include "firmwaredownloader.h"
#include "watchlogendpoint.h"
#include "core.h"
#include "platforminterface.h"
#include "ziphelper.h"
#include "dataloggingendpoint.h"
#include "devconnection.h"
#include "timelinemanager.h"

#include "QDir"
#include <QDateTime>
#include <QStandardPaths>
#include <QSettings>
#include <QTimeZone>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

Pebble::Pebble(const QBluetoothAddress &address, QObject *parent):
    QObject(parent),
    m_address(address)
{
    QString watchPath = m_address.toString().replace(':', '_');
    m_storagePath = QStandardPaths::writableLocation(QStandardPaths::DataLocation) + "/" + watchPath + "/";
    m_imagePath = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation) + "/screenshots/Pebble/";

    m_connection = new WatchConnection(this);
    QObject::connect(m_connection, &WatchConnection::watchConnected, this, &Pebble::onPebbleConnected);
    QObject::connect(m_connection, &WatchConnection::watchDisconnected, this, &Pebble::onPebbleDisconnected);
    QObject::connect(Core::instance()->platform(), &PlatformInterface::timeChanged, this, &Pebble::syncTime);

    m_connection->registerEndpointHandler(WatchConnection::EndpointVersion, this, "pebbleVersionReceived");
    m_connection->registerEndpointHandler(WatchConnection::EndpointPhoneVersion, this, "phoneVersionAsked");
    m_connection->registerEndpointHandler(WatchConnection::EndpointFactorySettings, this, "factorySettingsReceived");

    m_dataLogEndpoint = new DataLoggingEndpoint(this, m_connection);

    m_notificationEndpoint = new NotificationEndpoint(this, m_connection);
    QObject::connect(Core::instance()->platform(), &PlatformInterface::newTimelinePin, this, &Pebble::insertPin);
    QObject::connect(Core::instance()->platform(), &PlatformInterface::delTimelinePin, this, &Pebble::removePin);

    m_musicEndpoint = new MusicEndpoint(this, m_connection);
    m_musicEndpoint->setMusicMetadata(Core::instance()->platform()->musicMetaData());
    QObject::connect(m_musicEndpoint, &MusicEndpoint::musicControlPressed, Core::instance()->platform(), &PlatformInterface::sendMusicControlCommand);
    QObject::connect(Core::instance()->platform(), &PlatformInterface::musicMetadataChanged, m_musicEndpoint, &MusicEndpoint::setMusicMetadata);
    QObject::connect(Core::instance()->platform(), &PlatformInterface::musicPlayStateChanged, m_musicEndpoint, &MusicEndpoint::writePlayState);

    m_phoneCallEndpoint = new PhoneCallEndpoint(this, m_connection);
    QObject::connect(m_phoneCallEndpoint, &PhoneCallEndpoint::hangupCall, Core::instance()->platform(), &PlatformInterface::hangupCall);
    QObject::connect(Core::instance()->platform(), &PlatformInterface::incomingCall, m_phoneCallEndpoint, &PhoneCallEndpoint::incomingCall);
    QObject::connect(Core::instance()->platform(), &PlatformInterface::callStarted, m_phoneCallEndpoint, &PhoneCallEndpoint::callStarted);
    QObject::connect(Core::instance()->platform(), &PlatformInterface::callEnded, m_phoneCallEndpoint, &PhoneCallEndpoint::callEnded);

    m_appManager = new AppManager(this, m_connection);
    QObject::connect(m_appManager, &AppManager::appsChanged, this, &Pebble::installedAppsChanged);
    QObject::connect(m_appManager, &AppManager::idMismatchDetected, this, &Pebble::resetPebble);

    m_appMsgManager = new AppMsgManager(this, m_appManager, m_connection);
    m_jskitManager = new JSKitManager(this, m_connection, m_appManager, m_appMsgManager, this);
    QObject::connect(m_jskitManager, &JSKitManager::openURL, this, &Pebble::openURL);
    QObject::connect(m_jskitManager, &JSKitManager::appNotification, this, &Pebble::insertPin);
    QObject::connect(m_appMsgManager, &AppMsgManager::appStarted, this, &Pebble::appStarted);

    m_blobDB = new BlobDB(this, m_connection);
    QObject::connect(m_blobDB, &BlobDB::appInserted, this, &Pebble::appInstalled);

    m_timelineManager = new TimelineManager(this, m_connection);
    QObject::connect(m_timelineManager, &TimelineManager::muteSource, this, &Pebble::muteNotificationSource);
    QObject::connect(m_timelineManager, &TimelineManager::actionTriggered, Core::instance()->platform(), &PlatformInterface::actionTriggered);
    QObject::connect(m_timelineManager, &TimelineManager::removeNotification, Core::instance()->platform(), &PlatformInterface::removeNotification);

    m_appDownloader = new AppDownloader(m_storagePath, this);
    QObject::connect(m_appDownloader, &AppDownloader::downloadFinished, this, &Pebble::appDownloadFinished);

    m_screenshotEndpoint = new ScreenshotEndpoint(this, m_connection, this);
    QObject::connect(m_screenshotEndpoint, &ScreenshotEndpoint::screenshotAdded, this, &Pebble::screenshotAdded);
    QObject::connect(m_screenshotEndpoint, &ScreenshotEndpoint::screenshotRemoved, this, &Pebble::screenshotRemoved);

    m_firmwareDownloader = new FirmwareDownloader(this, m_connection);
    QObject::connect(m_firmwareDownloader, &FirmwareDownloader::updateAvailableChanged, this, &Pebble::slotUpdateAvailableChanged);
    QObject::connect(m_firmwareDownloader, &FirmwareDownloader::upgradingChanged, this, &Pebble::upgradingFirmwareChanged);
    QObject::connect(m_firmwareDownloader, &FirmwareDownloader::layoutsChanged, m_timelineManager, &TimelineManager::reloadLayouts);

    m_logEndpoint = new WatchLogEndpoint(this, m_connection);
    QObject::connect(m_logEndpoint, &WatchLogEndpoint::logsFetched, this, &Pebble::logsDumped);

    QSettings watchInfo(m_storagePath + "/watchinfo.conf", QSettings::IniFormat);
    m_model = (Model)watchInfo.value("watchModel", (int)ModelUnknown).toInt();

    QSettings settings(m_storagePath + "/appsettings.conf", QSettings::IniFormat);
    settings.beginGroup("activityParams");
    m_healthParams.setEnabled(settings.value("enabled").toBool());
    m_healthParams.setAge(settings.value("age").toUInt());
    m_healthParams.setHeight(settings.value("height").toInt());
    m_healthParams.setGender((HealthParams::Gender)settings.value("gender").toInt());
    m_healthParams.setWeight(settings.value("weight").toInt());
    m_healthParams.setMoreActive(settings.value("moreActive").toBool());
    m_healthParams.setSleepMore(settings.value("sleepMore").toBool());
    settings.endGroup();

    settings.beginGroup("unitsDistance");
    m_imperialUnits = settings.value("imperialUnits", false).toBool();
    settings.endGroup();

    settings.beginGroup("calendar");
    m_calendarSyncEnabled = settings.value("calendarSyncEnabled", true).toBool();
    settings.endGroup();

    settings.beginGroup("profileWhen");
    m_profileWhenConnected = settings.value("connected", "").toString();
    m_profileWhenDisconnected = settings.value("disconnected", "").toString();
    settings.endGroup();

    QObject::connect(m_connection, &WatchConnection::watchConnected, this, &Pebble::profileSwitchRequired);
    QObject::connect(m_connection, &WatchConnection::watchDisconnected, this, &Pebble::profileSwitchRequired);
    QObject::connect(this, &Pebble::profileConnectionSwitchChanged, this, &Pebble::profileSwitchRequired);

    m_devConnection = new DevConnection(this, m_connection);
    QObject::connect(m_devConnection, &DevConnection::serverStateChanged, this, &Pebble::devConServerStateChanged);
    QObject::connect(m_devConnection, &DevConnection::cloudStateChanged, this, &Pebble::devConCloudStateChanged);
    settings.beginGroup("devConnection");
    // DeveloperConnection is a backdoor to the pebble, it has no authentication whatsoever.
    // Dont ever enable it automatically, only on-demand by explicit user request!!111
    //m_devConnection->setEnabled(settings.value("enabled", true).toBool());
    m_devConnection->setPort(settings.value("listenPort", 9000).toInt());
    m_devConnection->setCloudEnabled(settings.value("useCloud", true).toBool()); // not implemented yet
    settings.endGroup();

}

QBluetoothAddress Pebble::address() const
{
    return m_address;
}

QString Pebble::name() const
{
    return m_name;
}

void Pebble::setName(const QString &name)
{
    m_name = name;
}

QBluetoothLocalDevice::Pairing Pebble::pairingStatus() const
{
    QBluetoothLocalDevice dev;
    return dev.pairingStatus(m_address);
}

bool Pebble::connected() const
{
    return m_connection->isConnected() && !m_serialNumber.isEmpty();
}

void Pebble::connect()
{
    qDebug() << "Connecting to Pebble:" << m_name << m_address.toString();
    m_connection->connectPebble(m_address);
}

BlobDB * Pebble::blobdb() const
{
    return m_blobDB;
}

QDateTime Pebble::softwareBuildTime() const
{
    return m_softwareBuildTime;
}

QString Pebble::softwareVersion() const
{
    return m_softwareVersion;
}

QString Pebble::softwareCommitRevision() const
{
    return m_softwareCommitRevision;
}

HardwareRevision Pebble::hardwareRevision() const
{
    return m_hardwareRevision;
}

Model Pebble::model() const
{
    return m_model;
}

void Pebble::setHardwareRevision(HardwareRevision hardwareRevision)
{
    m_hardwareRevision = hardwareRevision;
    switch (m_hardwareRevision) {
    case HardwareRevisionUNKNOWN:
        m_hardwarePlatform = HardwarePlatformUnknown;
        break;
    case HardwareRevisionTINTIN_EV1:
    case HardwareRevisionTINTIN_EV2:
    case HardwareRevisionTINTIN_EV2_3:
    case HardwareRevisionTINTIN_EV2_4:
    case HardwareRevisionTINTIN_V1_5:
    case HardwareRevisionBIANCA:
    case HardwareRevisionTINTIN_BB:
    case HardwareRevisionTINTIN_BB2:
        m_hardwarePlatform = HardwarePlatformAplite;
        break;
    case HardwareRevisionSNOWY_EVT2:
    case HardwareRevisionSNOWY_DVT:
    case HardwareRevisionBOBBY_SMILES:
    case HardwareRevisionSNOWY_BB:
    case HardwareRevisionSNOWY_BB2:
        m_hardwarePlatform = HardwarePlatformBasalt;
        break;
    case HardwareRevisionSPALDING_EVT:
    case HardwareRevisionSPALDING:
    case HardwareRevisionSPALDING_BB2:
        m_hardwarePlatform = HardwarePlatformChalk;
        break;
    }
}

HardwarePlatform Pebble::hardwarePlatform() const
{
    return m_hardwarePlatform;
}

QString Pebble::serialNumber() const
{
    return m_serialNumber;
}

QString Pebble::language() const
{
    return m_language;
}

Capabilities Pebble::capabilities() const
{
    return m_capabilities;
}

bool Pebble::isUnfaithful() const
{
    return m_isUnfaithful;
}

bool Pebble::recovery() const
{
    return m_recovery;
}

bool Pebble::upgradingFirmware() const
{
    return m_firmwareDownloader->upgrading();
}

bool Pebble::devConEnabled() const
{
    return m_devConnection->enabled();
}
void Pebble::setDevConEnabled(bool enabled)
{
    m_devConnection->setEnabled(enabled);
}
quint16 Pebble::devConListenPort() const
{
    return m_devConnection->listenPort();
}
void Pebble::setDevConListenPort(quint16 port)
{
    m_devConnection->setPort(port);
}
bool Pebble::devConServerState() const
{
    return m_devConnection->serverState();
}
bool Pebble::devConCloudEnabled() const
{
    return m_devConnection->cloudEnabled();
}
void Pebble::setDevConCloudEnabled(bool enabled)
{
    m_devConnection->setCloudEnabled(enabled);
}
bool Pebble::devConCloudState() const
{
    return m_devConnection->cloudState();
}

void Pebble::setHealthParams(const HealthParams &healthParams)
{
    m_healthParams = healthParams;
    m_blobDB->setHealthParams(healthParams);
    emit healtParamsChanged();

    QSettings healthSettings(m_storagePath + "/appsettings.conf", QSettings::IniFormat);
    healthSettings.beginGroup("activityParams");
    healthSettings.setValue("enabled", m_healthParams.enabled());
    healthSettings.setValue("age", m_healthParams.age());
    healthSettings.setValue("height", m_healthParams.height());
    healthSettings.setValue("gender", m_healthParams.gender());
    healthSettings.setValue("weight", m_healthParams.weight());
    healthSettings.setValue("moreActive", m_healthParams.moreActive());
    healthSettings.setValue("sleepMore", m_healthParams.sleepMore());

}

HealthParams Pebble::healthParams() const
{
    return m_healthParams;
}

void Pebble::setImperialUnits(bool imperial)
{
    m_imperialUnits = imperial;
    m_blobDB->setUnits(imperial);
    emit imperialUnitsChanged();

    QSettings settings(m_storagePath + "/appsettings.conf", QSettings::IniFormat);
    settings.beginGroup("unitsDistance");
    settings.setValue("enabled", m_imperialUnits);
}

bool Pebble::imperialUnits() const
{
    return m_imperialUnits;
}

void Pebble::dumpLogs(const QString &fileName) const
{
    m_logEndpoint->fetchLogs(fileName);
}

QString Pebble::storagePath() const
{
    return m_storagePath;
}

QString Pebble::imagePath() const
{
    return m_imagePath;
}

 QVariantMap Pebble::notificationsFilter() const
{
    QVariantMap ret;
    QString settingsFile = m_storagePath + "/notifications.conf";
    QSettings s(settingsFile, QSettings::IniFormat);

    foreach (const QString &group, s.childGroups()) {
        s.beginGroup(group);
        QVariantMap notif;
        notif.insert("enabled", Pebble::NotificationFilter(s.value("enabled").toInt()));
        notif.insert("icon", s.value("icon").toString());
        notif.insert("name", s.value("name").toString());
        ret.insert(group, notif);
        s.endGroup();
    }
    return ret;
}

void Pebble::setNotificationFilter(const QString &sourceId, const NotificationFilter enabled)
{
    QString settingsFile = m_storagePath + "/notifications.conf";
    QSettings s(settingsFile, QSettings::IniFormat);
    s.beginGroup(sourceId);
    if (s.value("enabled").toInt() != enabled) {
        s.setValue("enabled", enabled);
        emit notificationFilterChanged(sourceId, s.value("name").toString(), s.value("icon").toString(), enabled);
    }
    s.endGroup();
}

void Pebble::forgetNotificationFilter(const QString &sourceId) {
    if (sourceId.isEmpty()) return; // don't remove everything by accident
    QString settingsFile = m_storagePath + "/notifications.conf";
    QSettings s(settingsFile, QSettings::IniFormat);
    s.remove(sourceId);
    emit notificationFilterChanged(sourceId, "", "", NotificationForgotten);
}

void Pebble::setNotificationFilter(const QString &sourceId, const QString &name, const QString &icon, const NotificationFilter enabled)
{
    QString settingsFile = m_storagePath + "/notifications.conf";
    QSettings s(settingsFile, QSettings::IniFormat);
    qDebug() << "Setting" << sourceId << ":" << name << "with icon" << icon << "to" << enabled;
    bool changed = false;
    s.beginGroup(sourceId);
    if (s.value("enabled").toInt() != enabled) {
        s.setValue("enabled", enabled);
        changed = true;
    }

    if (!icon.isEmpty() && s.value("icon").toString() != icon) {
        s.setValue("icon", icon);
        changed = true;
    }
    else if (s.value("icon").toString().isEmpty()) {
        s.setValue("icon", findNotificationData(sourceId, "Icon"));
        changed = true;
    }

    if (!name.isEmpty() && s.value("name").toString() != name) {
        s.setValue("name", name);
        changed = true;
    }
    else if (s.value("name").toString().isEmpty()) {
        s.setValue("name", findNotificationData(sourceId, "Name"));
        changed = true;
    }

    if (changed)
        emit notificationFilterChanged(sourceId, s.value("name").toString(), s.value("icon").toString(), enabled);
    s.endGroup();

}

QString Pebble::findNotificationData(const QString &sourceId, const QString &key)
{
    qDebug() << "Looking for notification" << key << "for" << sourceId << "in launchers.";
    QStringList appsDirs = QStandardPaths::standardLocations(QStandardPaths::ApplicationsLocation);
    foreach (const QString &appsDir, appsDirs) {
        QDir dir(appsDir);
        QFileInfoList entries = dir.entryInfoList({"*.desktop"});
        foreach (const QFileInfo &appFile, entries) {
            QSettings s(appFile.absoluteFilePath(), QSettings::IniFormat);
            s.beginGroup("Desktop Entry");
            if (appFile.baseName() == sourceId
                    || s.value("Exec").toString() == sourceId
                    || s.value("X-apkd-packageName").toString() == sourceId) {
                return s.value(key).toString();
            }
        }
    }
    return 0;
}

void Pebble::insertPin(const QJsonObject &json)
{
    QJsonObject pinObj(json);
    if(!pinObj.contains("guid")) {
        QUuid guid;
        if(pinObj.contains("id")) {
            guid = PlatformInterface::idToGuid(pinObj.value("id").toString());
        } else {
            guid = QUuid::createUuid();
            qWarning() << "Neither GUID nor ID field present, generating random, pin control will be limited";
        }
        pinObj.insert("guid",guid.toString().mid(1,36));
    }
    if(pinObj.contains("type") && pinObj.value("type").toString() == "notification") {
        QStringList dataSource = pinObj.value("dataSource").toString().split(":");
        if(dataSource.count() > 1) {
            QString parent = dataSource.takeLast();
            QString sourceId = dataSource.first();
            if(dataSource.count() > 1) { // Escape colon in the srcId
                sourceId = dataSource.join("%3A");
                pinObj.insert("dataSource",QString("%1:%2").arg(sourceId).arg(parent));
            }
            QVariantMap notifFilter = notificationsFilter().value(sourceId).toMap();
            NotificationFilter f = NotificationFilter(notifFilter.value("enabled", QVariant(NotificationEnabled)).toInt());
            if (f==NotificationDisabled || (f==Pebble::NotificationDisabledActive && Core::instance()->platform()->deviceIsActive())) {
                qDebug() << "Notifications for" << sourceId << "disabled.";
                Core::instance()->platform()->removeNotification(QUuid(pinObj.value("guid").toString()));
                return;
            }
            // In case it wasn't there before, make sure to write it to the config now so it will appear in the config app.
            setNotificationFilter(sourceId, pinObj.value("source").toString(), pinObj.value("sourceIcon").toString(), NotificationEnabled);
            // Build mute action so that we can mute event passing through this section
            QJsonArray actions = pinObj.value("actions").toArray();
            QJsonObject mute;
            mute.insert("type",QString("mute"));
            QString sender = pinObj.value("createNotification").toObject().value("layout").toObject().value("sender").toString();
            mute.insert("title",QString(sender.isEmpty()?"Mute":"Mute "+sender));
            actions.append(mute);
            pinObj.insert("actions",actions);
        }
    }
    if(!pinObj.contains("dataSource")) {
        QString parent = PlatformInterface::idToGuid("dbus").toString().mid(1,36);
        if(pinObj.contains("source")) {
            pinObj.insert("dataSource",QString("%1:%2").arg(pinObj.value("source").toString()).arg(parent));
        } else {
            pinObj.insert("dataSource",QString("genericDbus:%1").arg(parent));
        }
    }
    // These must be present for retention and updates
    if(!pinObj.contains("createTime"))
        pinObj.insert("createTime",QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    if(!pinObj.contains("updateTime"))
        pinObj.insert("updateTime",QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    qDebug() << "Inserting pin" << QJsonDocument(pinObj).toJson();
    m_timelineManager->insertTimelinePin(pinObj);
}
void Pebble::removePin(const QString &guid)
{
    m_timelineManager->removeTimelinePin(guid);
}

void Pebble::clearAppDB()
{
    m_blobDB->clearApps();
}

void Pebble::clearTimeline()
{
    m_timelineManager->clearTimeline(PlatformInterface::UUID);
}

void Pebble::syncCalendar()
{
    Core::instance()->platform()->syncOrganizer();
}

void Pebble::setCalendarSyncEnabled(bool enabled)
{
    if (m_calendarSyncEnabled == enabled) {
        return;
    }
    m_calendarSyncEnabled = enabled;
    emit calendarSyncEnabledChanged();

    if (!m_calendarSyncEnabled) {
        Core::instance()->platform()->stopOrganizer();
        m_timelineManager->clearTimeline(PlatformInterface::UUID);
    } else {
        Core::instance()->platform()->syncOrganizer();
    }

    QSettings settings(m_storagePath + "/appsettings.conf", QSettings::IniFormat);
    settings.beginGroup("calendar");
    settings.setValue("calendarSyncEnabled", m_calendarSyncEnabled);
    settings.endGroup();
}

bool Pebble::calendarSyncEnabled() const
{
    return m_calendarSyncEnabled;
}

QString Pebble::profileWhen(bool connected) const {
    if (connected)
        return m_profileWhenConnected;
    else
        return m_profileWhenDisconnected;
}

void Pebble::setProfileWhen(const bool connected, const QString &profile)
{
    QString *profileWhen = connected?&m_profileWhenConnected:&m_profileWhenDisconnected;
    if (profileWhen == profile) {
        return;
    }
    *profileWhen = profile;
    emit profileConnectionSwitchChanged(connected);

    QSettings settings(m_storagePath + "/appsettings.conf", QSettings::IniFormat);
    settings.beginGroup("profileWhen");

    settings.setValue(connected?"connected":"disconnected", *profileWhen);
    settings.endGroup();
}

void Pebble::installApp(const QString &id)
{
    m_appDownloader->downloadApp(id);
}

void Pebble::sideloadApp(const QString &packageFile)
{
    QString targetFile = packageFile;
    targetFile.remove("file://");

    QString id;
    QTemporaryDir td;
    if(td.isValid()) {
        if (!ZipHelper::unpackArchive(targetFile, td.path())) {
            qWarning() << "Error unpacking App zip file" << targetFile << "to" << td.path();
            return;
        }
        qDebug() << "Pre-scanning app" << td.path();
        AppInfo info(td.path());
        if (!info.isValid()) {
            qWarning() << "Error parsing App metadata" << targetFile << "at" << td.path();
            return;
        }
        if(installedAppIds().contains(info.uuid())) {
            id = appInfo(info.uuid()).storeId(); // Existing app, upgrade|downgrade|overwrite
        } else {
            id = info.uuid().toString().mid(1,36); // Install new app under apps/uuid/
            QDir ad;
            if(!ad.mkpath(m_storagePath + "apps/" + id)) {
                qWarning() << "Cannot create app dir" << m_storagePath + "apps/" + id;
                return;
            }
        }

        if(!ZipHelper::unpackArchive(targetFile, m_storagePath + "apps/" + id)) {
                qWarning() << "Error unpacking App zip file" << targetFile << "to" << m_storagePath + "apps/" + id;
                return;
        }
        qDebug() << "Sideload package unpacked to" << m_storagePath + "apps/" + id;
        // Store the file. Sideloaded file will likely be of the same name/version
        QString newFile = m_storagePath + "apps/" + id + "/v"+info.versionLabel()+".pbw";
        if(QFile::exists(newFile))
            QFile::remove(newFile);
        QFile::rename(targetFile,newFile);
        appDownloadFinished(id);
    }
}

QList<QUuid> Pebble::installedAppIds()
{
    return m_appManager->appUuids();
}

void Pebble::setAppOrder(const QList<QUuid> &newList)
{
    m_appManager->setAppOrder(newList);
}

AppInfo Pebble::appInfo(const QUuid &uuid)
{
    return m_appManager->info(uuid);
}

void Pebble::removeApp(const QUuid &uuid)
{
    qDebug() << "Should remove app:" << uuid;
    m_blobDB->removeApp(m_appManager->info(uuid));
    m_appManager->removeApp(uuid);
}

void Pebble::launchApp(const QUuid &uuid)
{
    m_appMsgManager->launchApp(uuid);
}

void Pebble::requestConfigurationURL(const QUuid &uuid) {
    if (m_jskitManager->currentApp().uuid() == uuid) {
        m_jskitManager->showConfiguration();
    }
    else {
        m_jskitManager->setConfigurationId(uuid);
        m_appMsgManager->launchApp(uuid);
    }
}

void Pebble::configurationClosed(const QUuid &uuid, const QString &result)
{
    if (m_jskitManager->currentApp().uuid() == uuid) {
        m_jskitManager->handleWebviewClosed(result);
    }
}

void Pebble::requestScreenshot()
{
    m_screenshotEndpoint->requestScreenshot();
}

QStringList Pebble::screenshots() const
{
    return m_screenshotEndpoint->screenshots();
}

void Pebble::removeScreenshot(const QString &filename)
{
    m_screenshotEndpoint->removeScreenshot(filename);
}

bool Pebble::firmwareUpdateAvailable() const
{
    return m_firmwareDownloader->updateAvailable();
}

QString Pebble::candidateFirmwareVersion() const
{
    return m_firmwareDownloader->candidateVersion();
}

QString Pebble::firmwareReleaseNotes() const
{
    return m_firmwareDownloader->releaseNotes();
}

void Pebble::upgradeFirmware() const
{
    m_firmwareDownloader->performUpgrade();
}

void Pebble::onPebbleConnected()
{
    qDebug() << "Pebble connected:" << m_name;
    QByteArray data;
    WatchDataWriter w(&data);
    w.write<quint8>(0); // Command fetch
    QString message = "mfg_color";
    w.writeLE<quint8>(message.length());
    w.writeFixedString(message.length(), message);
    m_connection->writeToPebble(WatchConnection::EndpointFactorySettings, data);

    m_connection->writeToPebble(WatchConnection::EndpointVersion, QByteArray(1, 0));
}

void Pebble::onPebbleDisconnected()
{
    qDebug() << "Pebble disconnected:" << m_name;
    emit pebbleDisconnected();
}

void Pebble::profileSwitchRequired()
{
    QString *targetProfile = m_connection->isConnected()?&m_profileWhenConnected:&m_profileWhenDisconnected;
    if (targetProfile->isEmpty()) return;
    qDebug() << "Request Profile Switch: connected=" << m_connection->isConnected() << " profile=" << targetProfile;
     Core::instance()->platform()->setProfile(*targetProfile);
}

void Pebble::pebbleVersionReceived(const QByteArray &data)
{
    WatchDataReader wd(data);

    wd.skip(1);
    m_softwareBuildTime = QDateTime::fromTime_t(wd.read<quint32>());
    qDebug() << "Software Version build:" << m_softwareBuildTime;
    m_softwareVersion = wd.readFixedString(32);
    qDebug() << "Software Version string:" << m_softwareVersion;
    m_softwareCommitRevision = wd.readFixedString(8);
    qDebug() << "Software Version commit:" << m_softwareCommitRevision;

    m_recovery = wd.read<quint8>();
    qDebug() << "Recovery:" << m_recovery;
    HardwareRevision rev = (HardwareRevision)wd.read<quint8>();
    setHardwareRevision(rev);
    qDebug() << "HW Revision:" << rev;
    qDebug() << "Metadata Version:" << wd.read<quint8>();

    qDebug() << "Safe build:" << QDateTime::fromTime_t(wd.read<quint32>());
    qDebug() << "Safe version:" << wd.readFixedString(32);
    qDebug() << "safe commit:" << wd.readFixedString(8);
    qDebug() << "Safe recovery:" << wd.read<quint8>();
    qDebug() << "HW Revision:" << wd.read<quint8>();
    qDebug() << "Metadata Version:" << wd.read<quint8>();

    qDebug() << "BootloaderBuild" << QDateTime::fromTime_t(wd.read<quint32>());
    qDebug() << "hardwareRevision" << wd.readFixedString(9);
    m_serialNumber = wd.readFixedString(12);
    qDebug() << "serialnumber" << m_serialNumber;
    qDebug() << "BT address" << wd.readBytes(6).toHex();
    qDebug() << "CRC:" << wd.read<quint32>();
    qDebug() << "Resource timestamp:" << QDateTime::fromTime_t(wd.read<quint32>());
    m_language = wd.readFixedString(6);
    qDebug() << "Language" << m_language;
    qDebug() << "Language version" << wd.read<quint16>();
    // Capabilities is 64 bits but QFlags can only do 32 bits. lets split it into 2 * 32.
    // only 8 bits are used atm anyways.
    m_capabilities = QFlag(wd.readLE<quint32>());
    qDebug() << "Capabilities" << QString::number(m_capabilities, 16);
    qDebug() << "Capabilities" << wd.readLE<quint32>();
    m_isUnfaithful = wd.read<quint8>();
    qDebug() << "Is Unfaithful" << m_isUnfaithful;

    // This is useful for debugging
//    m_isUnfaithful = true;

    if (!m_recovery) {
        m_appManager->rescan();

        QSettings version(m_storagePath + "/watchinfo.conf", QSettings::IniFormat);
        if (version.value("syncedWithVersion").toString() != QStringLiteral(VERSION)) {
            m_isUnfaithful = true;
        }

        if (m_isUnfaithful) {
            qDebug() << "Pebble sync state unclear. Resetting Pebble watch.";
            resetPebble();
        } else {
            Core::instance()->platform()->syncOrganizer();
            syncApps();
            m_blobDB->setHealthParams(m_healthParams);
            m_blobDB->setUnits(m_imperialUnits);
        }
        version.setValue("syncedWithVersion", QStringLiteral(VERSION));

        syncTime();
    }

    m_firmwareDownloader->checkForNewFirmware();
    emit pebbleConnected();

}

void Pebble::factorySettingsReceived(const QByteArray &data)
{
    qDebug() << "have factory settings" << data.toHex();

    WatchDataReader reader(data);
    quint8 status = reader.read<quint8>();
    quint8 len = reader.read<quint8>();

    if (status != 0x01 && len != 0x04) {
        qWarning() << "Unexpected data reading factory settings";
        return;
    }
    m_model = (Model)reader.read<quint32>();
    QSettings s(m_storagePath + "/watchinfo.conf", QSettings::IniFormat);
    s.setValue("watchModel", m_model);
}

void Pebble::phoneVersionAsked(const QByteArray &data)
{
    Q_UNUSED(data);
    QByteArray res;

    Capabilities sessionCap(CapabilityHealth
                            | CapabilityAppRunState
                            | CapabilityUpdatedMusicProtocol | CapabilityInfiniteLogDumping | Capability8kAppMessages);

    quint32 platformFlags = 16 | 32 | OSAndroid;

    WatchDataWriter writer(&res);
    writer.writeLE<quint8>(0x01); // ok
    writer.writeLE<quint32>(0xFFFFFFFF);
    writer.writeLE<quint32>(sessionCap);
    writer.write<quint32>(platformFlags);
    writer.write<quint8>(2); // response version
    writer.write<quint8>(3); // major version
    writer.write<quint8>(0); // minor version
    writer.write<quint8>(0); // bugfix version
    writer.writeLE<quint64>(sessionCap);

    qDebug() << "sending phone version" << res.toHex();

    m_connection->writeToPebble(WatchConnection::EndpointPhoneVersion, res);
}

void Pebble::appDownloadFinished(const QString &id)
{
    QUuid uuid = m_appManager->scanApp(m_storagePath + "/apps/" + id);
    if (uuid.isNull()) {
        qWarning() << "Error scanning downloaded app. Won't install on watch";
        return;
    }
    // Stop running pebble app to avoid race-condition with JSkit stop
    if (m_jskitManager->currentApp().uuid() == uuid) {
        m_appMsgManager->closeApp(uuid);
    }
    // Force app replacement to allow update from store/sdk
    m_blobDB->insertAppMetaData(m_appManager->info(uuid),true);
    // The app will be re-launched here anyway
    m_pendingInstallations.append(uuid);
}

void Pebble::appInstalled(const QUuid &uuid) {
    if (m_pendingInstallations.contains(uuid)) {
        m_appMsgManager->launchApp(uuid);
    }

    if (uuid == m_lastSyncedAppUuid) {
        m_lastSyncedAppUuid = QUuid();

        m_appManager->setAppOrder(m_appManager->appUuids());
        QSettings settings(m_storagePath + "/appsettings.conf", QSettings::IniFormat);
        if (settings.contains("watchface")) {
            m_appMsgManager->launchApp(settings.value("watchface").toUuid());
        }
    }
}

void Pebble::appStarted(const QUuid &uuid)
{
    AppInfo info = m_appManager->info(uuid);
    if (info.isWatchface()) {
        QSettings settings(m_storagePath + "/appsettings.conf", QSettings::IniFormat);
        settings.setValue("watchface", uuid.toString());
    }
}

void Pebble::muteNotificationSource(const QString &source)
{
    qDebug() << "Request to mute" << source;
    setNotificationFilter(source, NotificationDisabled);
}

void Pebble::resetPebble()
{
    clearTimeline();
    Core::instance()->platform()->syncOrganizer();

    clearAppDB();
    syncApps();
}

void Pebble::syncApps()
{
    QUuid lastSyncedAppUuid;
    foreach (const QUuid &appUuid, m_appManager->appUuids()) {
        if (!m_appManager->info(appUuid).isSystemApp()) {
            qDebug() << "Inserting app" << m_appManager->info(appUuid).shortName() << "into BlobDB";
            m_blobDB->insertAppMetaData(m_appManager->info(appUuid));
            m_lastSyncedAppUuid = appUuid;
        }
    }
}

void Pebble::syncTime()
{
    TimeMessage msg(TimeMessage::TimeOperationSetUTC);
    qDebug() << "Syncing Time" << QDateTime::currentDateTime() << msg.serialize().toHex();
    m_connection->writeToPebble(WatchConnection::EndpointTime, msg.serialize());
}

void Pebble::slotUpdateAvailableChanged()
{
    qDebug() << "update available" << m_firmwareDownloader->updateAvailable() << m_firmwareDownloader->candidateVersion();
    if (m_firmwareDownloader->updateAvailable()) {
        QJsonObject pin;
        pin.insert("id",QString("PebbleFirmware.%1").arg(QDateTime::currentDateTimeUtc().toMSecsSinceEpoch()));
        pin.insert("type",QString("notification"));
        pin.insert("source",QString("Pebble Firmware Updates"));
        pin.insert("dataSource",QString("PebbleFirmware:%1").arg(PlatformInterface::SysID));
        QJsonObject layout;
        layout.insert("title", QString("Pebble firmware %1 available").arg(m_firmwareDownloader->candidateVersion()));
        layout.insert("body",m_firmwareDownloader->releaseNotes());
        layout.insert("type",QString("genericNotification"));
        layout.insert("tinyIcon",QString("system://images/NOTIFICATION_FLAG"));
        pin.insert("layout",layout);
        insertPin(pin);

        m_connection->systemMessage(WatchConnection::SystemMessageFirmwareAvailable);
    }
    emit updateAvailableChanged();
}


TimeMessage::TimeMessage(TimeMessage::TimeOperation operation) :
    m_operation(operation)
{

}
QByteArray TimeMessage::serialize() const
{
    QByteArray ret;
    WatchDataWriter writer(&ret);
    writer.write<quint8>(m_operation);
    switch (m_operation) {
    case TimeOperationSetLocaltime:
        writer.writeLE<quint32>(QDateTime::currentMSecsSinceEpoch() / 1000);
        break;
    case TimeOperationSetUTC:
        writer.write<quint32>(QDateTime::currentDateTime().toMSecsSinceEpoch() / 1000);
        writer.write<qint16>(QDateTime::currentDateTime().offsetFromUtc() / 60);
        writer.writePascalString(QDateTime::currentDateTime().timeZone().displayName(QTimeZone::StandardTime));
        break;
    default:
        ;
    }
    return ret;
}
