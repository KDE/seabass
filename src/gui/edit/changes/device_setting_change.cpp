// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/device_setting_change.hpp"

#include <filesystem>

#include "gui/edit/save_context.hpp"
#include "infrastructure/rekordbox/rekordbox_settings_writer.hpp"

namespace seabass::gui
{

namespace fs = std::filesystem;

DeviceSettingChange::DeviceSettingChange(QString pioneerRoot, QString fileName, QString fieldLabel, QString oldValue,
                                          QString optionName)
    : m_pioneerRoot(std::move(pioneerRoot)),
      m_fileName(std::move(fileName)),
      m_fieldLabel(std::move(fieldLabel)),
      m_oldValue(std::move(oldValue)),
      m_optionName(std::move(optionName))
{
}

QString DeviceSettingChange::id() const
{
    return "settings:" + m_fileName + ":" + m_fieldLabel;
}

QString DeviceSettingChange::description() const
{
    return QStringLiteral("\"%1\": %2 -> %3 (%4)").arg(m_fieldLabel, m_oldValue, m_optionName, m_fileName);
}

QString DeviceSettingChange::verb() const
{
    return QStringLiteral("saved");
}

QString DeviceSettingChange::unit() const
{
    return QStringLiteral("settings");
}

QStringList DeviceSettingChange::formatsTouched() const
{
    return {};
}

// One settings file, named entirely from members. apply() still calls
// backupOnce() on the same path -- that is the fallback, and it skips a
// file the upfront pass already covered.
std::vector<BackupTarget> DeviceSettingChange::filesToBackup(SaveContext &ctx) const
{
    (void)ctx;
    const std::string filePath = m_pioneerRoot.toStdString() + "/" + m_fileName.toStdString();
    if (!fs::exists(filePath)) {
        return {};  // apply() reports the missing file; do not record one that is not there
    }
    return {{filePath, "device-settings"}};
}

ChangeOutcome DeviceSettingChange::apply(SaveContext &ctx)
{
    std::string filePath = m_pioneerRoot.toStdString() + "/" + m_fileName.toStdString();
    if (!fs::exists(filePath)) {
        return ChangeOutcome::failure("Settings file not found: " + m_fileName);
    }
    ctx.backupOnce(filePath, "device-settings");
    bool ok = infrastructure::rekordbox::writeDeviceSettingField(m_pioneerRoot.toStdString(),
                                                                 m_fileName.toStdString(),
                                                                 m_fieldLabel.toStdString(),
                                                                 m_optionName.toStdString());
    if (!ok) {
        return ChangeOutcome::failure("Could not save \"" + m_fieldLabel
                                      + "\" -- the file wasn't in the expected format.");
    }
    ctx.log().record("device-settings: set \"" + m_fieldLabel.toStdString() + "\" to " + m_optionName.toStdString()
                     + " in " + m_fileName.toStdString());
    return ChangeOutcome::success();
}

}  // namespace seabass::gui
