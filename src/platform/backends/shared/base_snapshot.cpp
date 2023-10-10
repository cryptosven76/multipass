/*
 * Copyright (C) Canonical, Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "base_snapshot.h"
#include "daemon/vm_specs.h" // TODO@snapshots move this

#include <multipass/id_mappings.h> // TODO@snapshots may be able to drop after extracting JSON utilities
#include <multipass/json_utils.h>
#include <multipass/vm_mount.h>

#include <scope_guard.hpp>

#include <QJsonArray> // TODO@snapshots may be able to drop after extracting JSON utilities
#include <QString>

#include <QFile>
#include <QTemporaryDir>
#include <stdexcept>

namespace mp = multipass;

namespace
{
constexpr auto snapshot_extension = "snapshot.json";
constexpr auto index_digits = 4; // these two go together
constexpr auto max_snapshots = 1000;
const auto snapshot_template = QStringLiteral("@%1"); /* avoid colliding with suspension snapshots; prefix with a char
                                                         that can't be part of the name, to avoid confusion */

QString derive_index_string(int index)
{
    return QString{"%1"}.arg(index, index_digits, 10, QLatin1Char('0'));
}

QString derive_snapshot_filename(const QString& index, const QString& name)
{
    return QString{"%1-%2.%3"}.arg(index, name, snapshot_extension);
}

std::unordered_map<std::string, mp::VMMount> load_mounts(const QJsonArray& json)
{
    std::unordered_map<std::string, mp::VMMount> mounts;
    for (const auto& entry : json)
    {
        mp::id_mappings uid_mappings;
        mp::id_mappings gid_mappings;

        auto target_path = entry.toObject()["target_path"].toString().toStdString();
        auto source_path = entry.toObject()["source_path"].toString().toStdString();

        for (QJsonValueRef uid_entry : entry.toObject()["uid_mappings"].toArray())
        {
            uid_mappings.push_back(
                {uid_entry.toObject()["host_uid"].toInt(), uid_entry.toObject()["instance_uid"].toInt()});
        }

        for (QJsonValueRef gid_entry : entry.toObject()["gid_mappings"].toArray())
        {
            gid_mappings.push_back(
                {gid_entry.toObject()["host_gid"].toInt(), gid_entry.toObject()["instance_gid"].toInt()});
        }

        uid_mappings = mp::unique_id_mappings(uid_mappings);
        gid_mappings = mp::unique_id_mappings(gid_mappings);
        auto mount_type = mp::VMMount::MountType(entry.toObject()["mount_type"].toInt());

        mp::VMMount mount{source_path, gid_mappings, uid_mappings, mount_type};
        mounts[target_path] = mount;
    }

    return mounts;
}

std::shared_ptr<mp::Snapshot> find_parent(const QJsonObject& json, mp::VirtualMachine& vm)
{
    auto parent_name = json["parent"].toString().toStdString();
    try
    {
        return parent_name.empty() ? nullptr : vm.get_snapshot(parent_name);
    }
    catch (std::out_of_range&)
    {
        throw std::runtime_error{fmt::format("Missing snapshot parent. Snapshot name: {}; parent name: {}",
                                             json["name"].toString(),
                                             parent_name)};
    }
}
} // namespace

mp::BaseSnapshot::BaseSnapshot(const std::string& name,    // NOLINT(modernize-pass-by-value)
                               const std::string& comment, // NOLINT(modernize-pass-by-value)
                               std::shared_ptr<Snapshot> parent,
                               int index,
                               const QDir& storage_dir,
                               const QDateTime& creation_timestamp,
                               int num_cores,
                               MemorySize mem_size,
                               MemorySize disk_space,
                               VirtualMachine::State state,
                               std::unordered_map<std::string, VMMount> mounts,
                               QJsonObject metadata)
    : name{name},
      comment{comment},
      parent{std::move(parent)},
      index{index},
      storage_dir{storage_dir},
      creation_timestamp{creation_timestamp},
      num_cores{num_cores},
      mem_size{mem_size},
      disk_space{disk_space},
      state{state},
      mounts{std::move(mounts)},
      metadata{std::move(metadata)}
{
    assert(index > 0 && "snapshot indices need to start at 1");

    if (name.empty())
        throw std::runtime_error{"Snapshot names cannot be empty"};
    if (num_cores < 1)
        throw std::runtime_error{fmt::format("Invalid number of cores for snapshot: {}", num_cores)};
    if (auto mem_bytes = mem_size.in_bytes(); mem_bytes < 1)
        throw std::runtime_error{fmt::format("Invalid memory size for snapshot: {}", mem_bytes)};
    if (auto disk_bytes = disk_space.in_bytes(); disk_bytes < 1)
        throw std::runtime_error{fmt::format("Invalid disk size for snapshot: {}", disk_bytes)};
}

mp::BaseSnapshot::BaseSnapshot(const std::string& name,
                               const std::string& comment,
                               std::shared_ptr<Snapshot> parent,
                               const VMSpecs& specs,
                               VirtualMachine& vm)
    : BaseSnapshot{name,
                   comment,
                   std::move(parent),
                   vm.get_snapshot_count() + 1,
                   vm.instance_directory(),
                   QDateTime::currentDateTimeUtc(),
                   specs.num_cores,
                   specs.mem_size,
                   specs.disk_space,
                   specs.state,
                   specs.mounts,
                   specs.metadata}
{
}

mp::BaseSnapshot::BaseSnapshot(const QJsonObject& json, VirtualMachine& vm)
    : BaseSnapshot(InnerJsonTag{}, json["snapshot"].toObject(), vm)
{
}

mp::BaseSnapshot::BaseSnapshot(InnerJsonTag, const QJsonObject& json, VirtualMachine& vm)
    : BaseSnapshot{
          json["name"].toString().toStdString(),                                           // name
          json["comment"].toString().toStdString(),                                        // comment
          find_parent(json, vm),                                                           // parent
          json["index"].toInt(),                                                           // index
          vm.instance_directory(),                                                         // storage_dir
          QDateTime::fromString(json["creation_timestamp"].toString(), Qt::ISODateWithMs), // creation_timestamp
          json["num_cores"].toInt(),                                                       // num_cores
          MemorySize{json["mem_size"].toString().toStdString()},                           // mem_size
          MemorySize{json["disk_space"].toString().toStdString()},                         // disk_space
          static_cast<mp::VirtualMachine::State>(json["state"].toInt()),                   // state
          load_mounts(json["mounts"].toArray()),                                           // mounts
          json["metadata"].toObject()}                                                     // metadata
{
}

QJsonObject mp::BaseSnapshot::serialize() const
{
    QJsonObject ret, snapshot{};
    const std::unique_lock lock{mutex};

    snapshot.insert("name", QString::fromStdString(name));
    snapshot.insert("comment", QString::fromStdString(comment));
    snapshot.insert("parent", QString::fromStdString(get_parents_name()));
    snapshot.insert("index", index);
    snapshot.insert("creation_timestamp", creation_timestamp.toString(Qt::ISODateWithMs));
    snapshot.insert("num_cores", num_cores);
    snapshot.insert("mem_size", QString::number(mem_size.in_bytes()));
    snapshot.insert("disk_space", QString::number(disk_space.in_bytes()));
    snapshot.insert("state", static_cast<int>(state));
    snapshot.insert("metadata", metadata);

    // Extract mount serialization
    QJsonArray json_mounts;
    for (const auto& mount : mounts)
    {
        QJsonObject entry;
        entry.insert("source_path", QString::fromStdString(mount.second.source_path));
        entry.insert("target_path", QString::fromStdString(mount.first));

        QJsonArray uid_mappings;

        for (const auto& map : mount.second.uid_mappings)
        {
            QJsonObject map_entry;
            map_entry.insert("host_uid", map.first);
            map_entry.insert("instance_uid", map.second);

            uid_mappings.append(map_entry);
        }

        entry.insert("uid_mappings", uid_mappings);

        QJsonArray gid_mappings;

        for (const auto& map : mount.second.gid_mappings)
        {
            QJsonObject map_entry;
            map_entry.insert("host_gid", map.first);
            map_entry.insert("instance_gid", map.second);

            gid_mappings.append(map_entry);
        }

        entry.insert("gid_mappings", gid_mappings);

        entry.insert("mount_type", static_cast<int>(mount.second.mount_type));
        json_mounts.append(entry);
    }

    snapshot.insert("mounts", json_mounts);
    ret.insert("snapshot", snapshot);

    return ret;
}

void mp::BaseSnapshot::persist() const
{
    const std::unique_lock lock{mutex};

    const auto snapshot_filename = derive_snapshot_filename(derive_index_string(index), QString::fromStdString(name));
    auto snapshot_filepath = storage_dir.filePath(snapshot_filename);
    MP_JSONUTILS.write_json(serialize(), snapshot_filepath);
}

QString mp::BaseSnapshot::derive_id() const
{
    return snapshot_template.arg(get_name().c_str());
}

void mp::BaseSnapshot::erase_helper()
{
    // Remove snapshot file
    QTemporaryDir tmp_dir{};
    if (!tmp_dir.isValid())
        throw std::runtime_error{"Could not create temporary directory"};

    const auto snapshot_filename = derive_snapshot_filename(
        derive_index_string(index), QString::fromStdString(name)); // TODO@ricab turn into method
    auto snapshot_filepath = storage_dir.filePath(snapshot_filename);
    auto deleting_filepath = tmp_dir.filePath(snapshot_filename);

    if (!QFile{snapshot_filepath}.rename(deleting_filepath)) // TODO@ricab use fileops
        throw std::runtime_error{
            fmt::format("Failed to move snapshot file to temporary destination: {}", deleting_filepath)};

    auto rollback_snapshot_file = sg::make_scope_guard([&deleting_filepath, &snapshot_filepath]() noexcept {
        QFile{deleting_filepath}.rename(snapshot_filepath); // best effort, ignore return
    });

    erase_impl();
    rollback_snapshot_file.dismiss();
}
