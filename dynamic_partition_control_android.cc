//
// Copyright (C) 2018 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "update_engine/dynamic_partition_control_android.h"

#include <chrono>  // NOLINT(build/c++11) - using libsnapshot / liblp API
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <android-base/properties.h>
#include <android-base/strings.h>
#include <base/files/file_util.h>
#include <base/logging.h>
#include <base/strings/string_util.h>
#include <bootloader_message/bootloader_message.h>
#include <fs_mgr.h>
#include <fs_mgr_dm_linear.h>
#include <fs_mgr_overlayfs.h>
#include <libdm/dm.h>
#include <libsnapshot/snapshot.h>

#include "update_engine/cleanup_previous_update_action.h"
#include "update_engine/common/boot_control_interface.h"
#include "update_engine/common/utils.h"
#include "update_engine/dynamic_partition_utils.h"

using android::base::GetBoolProperty;
using android::base::Join;
using android::dm::DeviceMapper;
using android::dm::DmDeviceState;
using android::fs_mgr::CreateLogicalPartition;
using android::fs_mgr::CreateLogicalPartitionParams;
using android::fs_mgr::DestroyLogicalPartition;
using android::fs_mgr::MetadataBuilder;
using android::fs_mgr::Partition;
using android::fs_mgr::PartitionOpener;
using android::fs_mgr::SlotSuffixForSlotNumber;
using android::snapshot::OptimizeSourceCopyOperation;
using android::snapshot::Return;
using android::snapshot::SnapshotManager;
using android::snapshot::UpdateState;

namespace chromeos_update_engine {

constexpr char kUseDynamicPartitions[] = "ro.boot.dynamic_partitions";
constexpr char kRetrfoitDynamicPartitions[] =
    "ro.boot.dynamic_partitions_retrofit";
constexpr char kVirtualAbEnabled[] = "ro.virtual_ab.enabled";
constexpr char kVirtualAbRetrofit[] = "ro.virtual_ab.retrofit";
// Map timeout for dynamic partitions.
constexpr std::chrono::milliseconds kMapTimeout{1000};
// Map timeout for dynamic partitions with snapshots. Since several devices
// needs to be mapped, this timeout is longer than |kMapTimeout|.
constexpr std::chrono::milliseconds kMapSnapshotTimeout{5000};

#ifdef __ANDROID_RECOVERY__
constexpr bool kIsRecovery = true;
#else
constexpr bool kIsRecovery = false;
#endif

DynamicPartitionControlAndroid::~DynamicPartitionControlAndroid() {
  Cleanup();
}

static FeatureFlag GetFeatureFlag(const char* enable_prop,
                                  const char* retrofit_prop) {
  bool retrofit = GetBoolProperty(retrofit_prop, false);
  bool enabled = GetBoolProperty(enable_prop, false);
  if (retrofit && !enabled) {
    LOG(ERROR) << retrofit_prop << " is true but " << enable_prop
               << " is not. These sysprops are inconsistent. Assume that "
               << enable_prop << " is true from now on.";
  }
  if (retrofit) {
    return FeatureFlag(FeatureFlag::Value::RETROFIT);
  }
  if (enabled) {
    return FeatureFlag(FeatureFlag::Value::LAUNCH);
  }
  return FeatureFlag(FeatureFlag::Value::NONE);
}

DynamicPartitionControlAndroid::DynamicPartitionControlAndroid()
    : dynamic_partitions_(
          GetFeatureFlag(kUseDynamicPartitions, kRetrfoitDynamicPartitions)),
      virtual_ab_(GetFeatureFlag(kVirtualAbEnabled, kVirtualAbRetrofit)) {
  if (GetVirtualAbFeatureFlag().IsEnabled()) {
    snapshot_ = SnapshotManager::New();
    CHECK(snapshot_ != nullptr) << "Cannot initialize SnapshotManager.";
  }
}

FeatureFlag DynamicPartitionControlAndroid::GetDynamicPartitionsFeatureFlag() {
  return dynamic_partitions_;
}

FeatureFlag DynamicPartitionControlAndroid::GetVirtualAbFeatureFlag() {
  return virtual_ab_;
}

bool DynamicPartitionControlAndroid::OptimizeOperation(
    const std::string& partition_name,
    const InstallOperation& operation,
    InstallOperation* optimized) {
  switch (operation.type()) {
    case InstallOperation::SOURCE_COPY:
      return target_supports_snapshot_ &&
             GetVirtualAbFeatureFlag().IsEnabled() &&
             mapped_devices_.count(partition_name +
                                   SlotSuffixForSlotNumber(target_slot_)) > 0 &&
             OptimizeSourceCopyOperation(operation, optimized);
      break;
    default:
      break;
  }
  return false;
}

bool DynamicPartitionControlAndroid::MapPartitionInternal(
    const std::string& super_device,
    const std::string& target_partition_name,
    uint32_t slot,
    bool force_writable,
    std::string* path) {
  CreateLogicalPartitionParams params = {
      .block_device = super_device,
      .metadata_slot = slot,
      .partition_name = target_partition_name,
      .force_writable = force_writable,
  };
  bool success = false;
  if (GetVirtualAbFeatureFlag().IsEnabled() && target_supports_snapshot_ &&
      force_writable) {
    // Only target partitions are mapped with force_writable. On Virtual
    // A/B devices, target partitions may overlap with source partitions, so
    // they must be mapped with snapshot.
    params.timeout_ms = kMapSnapshotTimeout;
    success = snapshot_->MapUpdateSnapshot(params, path);
  } else {
    params.timeout_ms = kMapTimeout;
    success = CreateLogicalPartition(params, path);
  }

  if (!success) {
    LOG(ERROR) << "Cannot map " << target_partition_name << " in "
               << super_device << " on device mapper.";
    return false;
  }
  LOG(INFO) << "Succesfully mapped " << target_partition_name
            << " to device mapper (force_writable = " << force_writable
            << "); device path at " << *path;
  mapped_devices_.insert(target_partition_name);
  return true;
}

bool DynamicPartitionControlAndroid::MapPartitionOnDeviceMapper(
    const std::string& super_device,
    const std::string& target_partition_name,
    uint32_t slot,
    bool force_writable,
    std::string* path) {
  DmDeviceState state = GetState(target_partition_name);
  if (state == DmDeviceState::ACTIVE) {
    if (mapped_devices_.find(target_partition_name) != mapped_devices_.end()) {
      if (GetDmDevicePathByName(target_partition_name, path)) {
        LOG(INFO) << target_partition_name
                  << " is mapped on device mapper: " << *path;
        return true;
      }
      LOG(ERROR) << target_partition_name << " is mapped but path is unknown.";
      return false;
    }
    // If target_partition_name is not in mapped_devices_ but state is ACTIVE,
    // the device might be mapped incorrectly before. Attempt to unmap it.
    // Note that for source partitions, if GetState() == ACTIVE, callers (e.g.
    // BootControlAndroid) should not call MapPartitionOnDeviceMapper, but
    // should directly call GetDmDevicePathByName.
    if (!UnmapPartitionOnDeviceMapper(target_partition_name)) {
      LOG(ERROR) << target_partition_name
                 << " is mapped before the update, and it cannot be unmapped.";
      return false;
    }
    state = GetState(target_partition_name);
    if (state != DmDeviceState::INVALID) {
      LOG(ERROR) << target_partition_name << " is unmapped but state is "
                 << static_cast<std::underlying_type_t<DmDeviceState>>(state);
      return false;
    }
  }
  if (state == DmDeviceState::INVALID) {
    return MapPartitionInternal(
        super_device, target_partition_name, slot, force_writable, path);
  }

  LOG(ERROR) << target_partition_name
             << " is mapped on device mapper but state is unknown: "
             << static_cast<std::underlying_type_t<DmDeviceState>>(state);
  return false;
}

bool DynamicPartitionControlAndroid::UnmapPartitionOnDeviceMapper(
    const std::string& target_partition_name) {
  if (DeviceMapper::Instance().GetState(target_partition_name) !=
      DmDeviceState::INVALID) {
    // Partitions at target slot on non-Virtual A/B devices are mapped as
    // dm-linear. Also, on Virtual A/B devices, system_other may be mapped for
    // preopt apps as dm-linear.
    // Call DestroyLogicalPartition to handle these cases.
    bool success = DestroyLogicalPartition(target_partition_name);

    // On a Virtual A/B device, |target_partition_name| may be a leftover from
    // a paused update. Clean up any underlying devices.
    if (GetVirtualAbFeatureFlag().IsEnabled()) {
      success &= snapshot_->UnmapUpdateSnapshot(target_partition_name);
    }

    if (!success) {
      LOG(ERROR) << "Cannot unmap " << target_partition_name
                 << " from device mapper.";
      return false;
    }
    LOG(INFO) << "Successfully unmapped " << target_partition_name
              << " from device mapper.";
  }
  mapped_devices_.erase(target_partition_name);
  return true;
}

void DynamicPartitionControlAndroid::UnmapAllPartitions() {
  if (mapped_devices_.empty()) {
    return;
  }
  // UnmapPartitionOnDeviceMapper removes objects from mapped_devices_, hence
  // a copy is needed for the loop.
  std::set<std::string> mapped = mapped_devices_;
  LOG(INFO) << "Destroying [" << Join(mapped, ", ") << "] from device mapper";
  for (const auto& partition_name : mapped) {
    ignore_result(UnmapPartitionOnDeviceMapper(partition_name));
  }
}

void DynamicPartitionControlAndroid::Cleanup() {
  UnmapAllPartitions();
  metadata_device_.reset();
}

bool DynamicPartitionControlAndroid::DeviceExists(const std::string& path) {
  return base::PathExists(base::FilePath(path));
}

android::dm::DmDeviceState DynamicPartitionControlAndroid::GetState(
    const std::string& name) {
  return DeviceMapper::Instance().GetState(name);
}

bool DynamicPartitionControlAndroid::GetDmDevicePathByName(
    const std::string& name, std::string* path) {
  return DeviceMapper::Instance().GetDmDevicePathByName(name, path);
}

std::unique_ptr<MetadataBuilder>
DynamicPartitionControlAndroid::LoadMetadataBuilder(
    const std::string& super_device, uint32_t source_slot) {
  return LoadMetadataBuilder(
      super_device, source_slot, BootControlInterface::kInvalidSlot);
}

std::unique_ptr<MetadataBuilder>
DynamicPartitionControlAndroid::LoadMetadataBuilder(
    const std::string& super_device,
    uint32_t source_slot,
    uint32_t target_slot) {
  std::unique_ptr<MetadataBuilder> builder;
  if (target_slot == BootControlInterface::kInvalidSlot) {
    builder =
        MetadataBuilder::New(PartitionOpener(), super_device, source_slot);
  } else {
    bool always_keep_source_slot = !target_supports_snapshot_;
    builder = MetadataBuilder::NewForUpdate(PartitionOpener(),
                                            super_device,
                                            source_slot,
                                            target_slot,
                                            always_keep_source_slot);
  }

  if (builder == nullptr) {
    LOG(WARNING) << "No metadata slot "
                 << BootControlInterface::SlotName(source_slot) << " in "
                 << super_device;
    return nullptr;
  }
  LOG(INFO) << "Loaded metadata from slot "
            << BootControlInterface::SlotName(source_slot) << " in "
            << super_device;
  return builder;
}

bool DynamicPartitionControlAndroid::StoreMetadata(
    const std::string& super_device,
    MetadataBuilder* builder,
    uint32_t target_slot) {
  auto metadata = builder->Export();
  if (metadata == nullptr) {
    LOG(ERROR) << "Cannot export metadata to slot "
               << BootControlInterface::SlotName(target_slot) << " in "
               << super_device;
    return false;
  }

  if (GetDynamicPartitionsFeatureFlag().IsRetrofit()) {
    if (!FlashPartitionTable(super_device, *metadata)) {
      LOG(ERROR) << "Cannot write metadata to " << super_device;
      return false;
    }
    LOG(INFO) << "Written metadata to " << super_device;
  } else {
    if (!UpdatePartitionTable(super_device, *metadata, target_slot)) {
      LOG(ERROR) << "Cannot write metadata to slot "
                 << BootControlInterface::SlotName(target_slot) << " in "
                 << super_device;
      return false;
    }
    LOG(INFO) << "Copied metadata to slot "
              << BootControlInterface::SlotName(target_slot) << " in "
              << super_device;
  }

  return true;
}

bool DynamicPartitionControlAndroid::GetDeviceDir(std::string* out) {
  // We can't use fs_mgr to look up |partition_name| because fstab
  // doesn't list every slot partition (it uses the slotselect option
  // to mask the suffix).
  //
  // We can however assume that there's an entry for the /misc mount
  // point and use that to get the device file for the misc
  // partition. This helps us locate the disk that |partition_name|
  // resides on. From there we'll assume that a by-name scheme is used
  // so we can just replace the trailing "misc" by the given
  // |partition_name| and suffix corresponding to |slot|, e.g.
  //
  //   /dev/block/platform/soc.0/7824900.sdhci/by-name/misc ->
  //   /dev/block/platform/soc.0/7824900.sdhci/by-name/boot_a
  //
  // If needed, it's possible to relax the by-name assumption in the
  // future by trawling /sys/block looking for the appropriate sibling
  // of misc and then finding an entry in /dev matching the sysfs
  // entry.

  std::string err, misc_device = get_bootloader_message_blk_device(&err);
  if (misc_device.empty()) {
    LOG(ERROR) << "Unable to get misc block device: " << err;
    return false;
  }

  if (!utils::IsSymlink(misc_device.c_str())) {
    LOG(ERROR) << "Device file " << misc_device << " for /misc "
               << "is not a symlink.";
    return false;
  }
  *out = base::FilePath(misc_device).DirName().value();
  return true;
}

bool DynamicPartitionControlAndroid::PreparePartitionsForUpdate(
    uint32_t source_slot,
    uint32_t target_slot,
    const DeltaArchiveManifest& manifest,
    bool update,
    uint64_t* required_size) {
  source_slot_ = source_slot;
  target_slot_ = target_slot;
  if (required_size != nullptr) {
    *required_size = 0;
  }

  if (fs_mgr_overlayfs_is_setup()) {
    // Non DAP devices can use overlayfs as well.
    LOG(WARNING)
        << "overlayfs overrides are active and can interfere with our "
           "resources.\n"
        << "run adb enable-verity to deactivate if required and try again.";
  }

  if (!GetDynamicPartitionsFeatureFlag().IsEnabled()) {
    return true;
  }

  if (target_slot == source_slot) {
    LOG(ERROR) << "Cannot call PreparePartitionsForUpdate on current slot.";
    return false;
  }

  // Although the current build supports dynamic partitions, the given payload
  // doesn't use it for target partitions. This could happen when applying a
  // retrofit update. Skip updating the partition metadata for the target slot.
  is_target_dynamic_ = !manifest.dynamic_partition_metadata().groups().empty();
  if (!is_target_dynamic_) {
    return true;
  }

  target_supports_snapshot_ =
      manifest.dynamic_partition_metadata().snapshot_enabled();

  if (GetVirtualAbFeatureFlag().IsEnabled()) {
    metadata_device_ = snapshot_->EnsureMetadataMounted();
    TEST_AND_RETURN_FALSE(metadata_device_ != nullptr);
  }

  if (!update)
    return true;

  bool delete_source = false;

  if (GetVirtualAbFeatureFlag().IsEnabled()) {
    // On Virtual A/B device, either CancelUpdate() or BeginUpdate() must be
    // called before calling UnmapUpdateSnapshot.
    // - If target_supports_snapshot_, PrepareSnapshotPartitionsForUpdate()
    //   calls BeginUpdate() which resets update state
    // - If !target_supports_snapshot_ or PrepareSnapshotPartitionsForUpdate
    //   failed in recovery, explicitly CancelUpdate().
    if (target_supports_snapshot_) {
      if (PrepareSnapshotPartitionsForUpdate(
              source_slot, target_slot, manifest, required_size)) {
        return true;
      }

      // Virtual A/B device doing Virtual A/B update in Android mode must use
      // snapshots.
      if (!IsRecovery()) {
        LOG(ERROR) << "PrepareSnapshotPartitionsForUpdate failed in Android "
                   << "mode";
        return false;
      }

      delete_source = true;
      LOG(INFO) << "PrepareSnapshotPartitionsForUpdate failed in recovery. "
                << "Attempt to overwrite existing partitions if possible";
    } else {
      // Downgrading to an non-Virtual A/B build or is secondary OTA.
      LOG(INFO) << "Using regular A/B on Virtual A/B because package disabled "
                << "snapshots.";
    }

    if (!snapshot_->CancelUpdate()) {
      LOG(ERROR) << "Cannot cancel previous update.";
      return false;
    }
  }

  return PrepareDynamicPartitionsForUpdate(
      source_slot, target_slot, manifest, delete_source);
}

bool DynamicPartitionControlAndroid::PrepareDynamicPartitionsForUpdate(
    uint32_t source_slot,
    uint32_t target_slot,
    const DeltaArchiveManifest& manifest,
    bool delete_source) {
  const std::string target_suffix = SlotSuffixForSlotNumber(target_slot);

  // Unmap all the target dynamic partitions because they would become
  // inconsistent with the new metadata.
  for (const auto& group : manifest.dynamic_partition_metadata().groups()) {
    for (const auto& partition_name : group.partition_names()) {
      if (!UnmapPartitionOnDeviceMapper(partition_name + target_suffix)) {
        return false;
      }
    }
  }

  std::string device_dir_str;
  if (!GetDeviceDir(&device_dir_str)) {
    return false;
  }
  base::FilePath device_dir(device_dir_str);
  auto source_device =
      device_dir.Append(GetSuperPartitionName(source_slot)).value();

  auto builder = LoadMetadataBuilder(source_device, source_slot, target_slot);
  if (builder == nullptr) {
    LOG(ERROR) << "No metadata at "
               << BootControlInterface::SlotName(source_slot);
    return false;
  }

  if (delete_source) {
    TEST_AND_RETURN_FALSE(
        DeleteSourcePartitions(builder.get(), source_slot, manifest));
  }

  if (!UpdatePartitionMetadata(builder.get(), target_slot, manifest)) {
    return false;
  }

  auto target_device =
      device_dir.Append(GetSuperPartitionName(target_slot)).value();
  return StoreMetadata(target_device, builder.get(), target_slot);
}

bool DynamicPartitionControlAndroid::PrepareSnapshotPartitionsForUpdate(
    uint32_t source_slot,
    uint32_t target_slot,
    const DeltaArchiveManifest& manifest,
    uint64_t* required_size) {
  if (!snapshot_->BeginUpdate()) {
    LOG(ERROR) << "Cannot begin new update.";
    return false;
  }
  auto ret = snapshot_->CreateUpdateSnapshots(manifest);
  if (!ret) {
    LOG(ERROR) << "Cannot create update snapshots: " << ret.string();
    if (required_size != nullptr &&
        ret.error_code() == Return::ErrorCode::NO_SPACE) {
      *required_size = ret.required_size();
    }
    return false;
  }
  return true;
}

std::string DynamicPartitionControlAndroid::GetSuperPartitionName(
    uint32_t slot) {
  return fs_mgr_get_super_partition_name(slot);
}

bool DynamicPartitionControlAndroid::UpdatePartitionMetadata(
    MetadataBuilder* builder,
    uint32_t target_slot,
    const DeltaArchiveManifest& manifest) {
  // If applying downgrade from Virtual A/B to non-Virtual A/B, the left-over
  // COW group needs to be deleted to ensure there are enough space to create
  // target partitions.
  builder->RemoveGroupAndPartitions(android::snapshot::kCowGroupName);

  const std::string target_suffix = SlotSuffixForSlotNumber(target_slot);
  DeleteGroupsWithSuffix(builder, target_suffix);

  uint64_t total_size = 0;
  for (const auto& group : manifest.dynamic_partition_metadata().groups()) {
    total_size += group.size();
  }

  std::string expr;
  uint64_t allocatable_space = builder->AllocatableSpace();
  if (!GetDynamicPartitionsFeatureFlag().IsRetrofit()) {
    allocatable_space /= 2;
    expr = "half of ";
  }
  if (total_size > allocatable_space) {
    LOG(ERROR) << "The maximum size of all groups with suffix " << target_suffix
               << " (" << total_size << ") has exceeded " << expr
               << "allocatable space for dynamic partitions "
               << allocatable_space << ".";
    return false;
  }

  // name of partition(e.g. "system") -> size in bytes
  std::map<std::string, uint64_t> partition_sizes;
  for (const auto& partition : manifest.partitions()) {
    partition_sizes.emplace(partition.partition_name(),
                            partition.new_partition_info().size());
  }

  for (const auto& group : manifest.dynamic_partition_metadata().groups()) {
    auto group_name_suffix = group.name() + target_suffix;
    if (!builder->AddGroup(group_name_suffix, group.size())) {
      LOG(ERROR) << "Cannot add group " << group_name_suffix << " with size "
                 << group.size();
      return false;
    }
    LOG(INFO) << "Added group " << group_name_suffix << " with size "
              << group.size();

    for (const auto& partition_name : group.partition_names()) {
      auto partition_sizes_it = partition_sizes.find(partition_name);
      if (partition_sizes_it == partition_sizes.end()) {
        // TODO(tbao): Support auto-filling partition info for framework-only
        // OTA.
        LOG(ERROR) << "dynamic_partition_metadata contains partition "
                   << partition_name << " but it is not part of the manifest. "
                   << "This is not supported.";
        return false;
      }
      uint64_t partition_size = partition_sizes_it->second;

      auto partition_name_suffix = partition_name + target_suffix;
      Partition* p = builder->AddPartition(
          partition_name_suffix, group_name_suffix, LP_PARTITION_ATTR_READONLY);
      if (!p) {
        LOG(ERROR) << "Cannot add partition " << partition_name_suffix
                   << " to group " << group_name_suffix;
        return false;
      }
      if (!builder->ResizePartition(p, partition_size)) {
        LOG(ERROR) << "Cannot resize partition " << partition_name_suffix
                   << " to size " << partition_size << ". Not enough space?";
        return false;
      }
      LOG(INFO) << "Added partition " << partition_name_suffix << " to group "
                << group_name_suffix << " with size " << partition_size;
    }
  }

  return true;
}

bool DynamicPartitionControlAndroid::FinishUpdate() {
  if (GetVirtualAbFeatureFlag().IsEnabled() &&
      snapshot_->GetUpdateState() == UpdateState::Initiated) {
    LOG(INFO) << "Snapshot writes are done.";
    return snapshot_->FinishedSnapshotWrites();
  }
  return true;
}

bool DynamicPartitionControlAndroid::GetPartitionDevice(
    const std::string& partition_name,
    uint32_t slot,
    uint32_t current_slot,
    std::string* device) {
  const auto& partition_name_suffix =
      partition_name + SlotSuffixForSlotNumber(slot);
  std::string device_dir_str;
  TEST_AND_RETURN_FALSE(GetDeviceDir(&device_dir_str));
  base::FilePath device_dir(device_dir_str);

  // When looking up target partition devices, treat them as static if the
  // current payload doesn't encode them as dynamic partitions. This may happen
  // when applying a retrofit update on top of a dynamic-partitions-enabled
  // build.
  if (GetDynamicPartitionsFeatureFlag().IsEnabled() &&
      (slot == current_slot || is_target_dynamic_)) {
    switch (GetDynamicPartitionDevice(
        device_dir, partition_name_suffix, slot, current_slot, device)) {
      case DynamicPartitionDeviceStatus::SUCCESS:
        return true;
      case DynamicPartitionDeviceStatus::TRY_STATIC:
        break;
      case DynamicPartitionDeviceStatus::ERROR:  // fallthrough
      default:
        return false;
    }
  }
  base::FilePath path = device_dir.Append(partition_name_suffix);
  if (!DeviceExists(path.value())) {
    LOG(ERROR) << "Device file " << path.value() << " does not exist.";
    return false;
  }

  *device = path.value();
  return true;
}

bool DynamicPartitionControlAndroid::IsSuperBlockDevice(
    const base::FilePath& device_dir,
    uint32_t current_slot,
    const std::string& partition_name_suffix) {
  std::string source_device =
      device_dir.Append(GetSuperPartitionName(current_slot)).value();
  auto source_metadata = LoadMetadataBuilder(source_device, current_slot);
  return source_metadata->HasBlockDevice(partition_name_suffix);
}

DynamicPartitionControlAndroid::DynamicPartitionDeviceStatus
DynamicPartitionControlAndroid::GetDynamicPartitionDevice(
    const base::FilePath& device_dir,
    const std::string& partition_name_suffix,
    uint32_t slot,
    uint32_t current_slot,
    std::string* device) {
  std::string super_device =
      device_dir.Append(GetSuperPartitionName(slot)).value();

  auto builder = LoadMetadataBuilder(super_device, slot);
  if (builder == nullptr) {
    LOG(ERROR) << "No metadata in slot "
               << BootControlInterface::SlotName(slot);
    return DynamicPartitionDeviceStatus::ERROR;
  }
  if (builder->FindPartition(partition_name_suffix) == nullptr) {
    LOG(INFO) << partition_name_suffix
              << " is not in super partition metadata.";

    if (IsSuperBlockDevice(device_dir, current_slot, partition_name_suffix)) {
      LOG(ERROR) << "The static partition " << partition_name_suffix
                 << " is a block device for current metadata."
                 << "It cannot be used as a logical partition.";
      return DynamicPartitionDeviceStatus::ERROR;
    }

    return DynamicPartitionDeviceStatus::TRY_STATIC;
  }

  if (slot == current_slot) {
    if (GetState(partition_name_suffix) != DmDeviceState::ACTIVE) {
      LOG(WARNING) << partition_name_suffix << " is at current slot but it is "
                   << "not mapped. Now try to map it.";
    } else {
      if (GetDmDevicePathByName(partition_name_suffix, device)) {
        LOG(INFO) << partition_name_suffix
                  << " is mapped on device mapper: " << *device;
        return DynamicPartitionDeviceStatus::SUCCESS;
      }
      LOG(ERROR) << partition_name_suffix << "is mapped but path is unknown.";
      return DynamicPartitionDeviceStatus::ERROR;
    }
  }

  bool force_writable = slot != current_slot;
  if (MapPartitionOnDeviceMapper(
          super_device, partition_name_suffix, slot, force_writable, device)) {
    return DynamicPartitionDeviceStatus::SUCCESS;
  }
  return DynamicPartitionDeviceStatus::ERROR;
}

void DynamicPartitionControlAndroid::set_fake_mapped_devices(
    const std::set<std::string>& fake) {
  mapped_devices_ = fake;
}

bool DynamicPartitionControlAndroid::IsRecovery() {
  return kIsRecovery;
}

static bool IsIncrementalUpdate(const DeltaArchiveManifest& manifest) {
  const auto& partitions = manifest.partitions();
  return std::any_of(partitions.begin(), partitions.end(), [](const auto& p) {
    return p.has_old_partition_info();
  });
}

bool DynamicPartitionControlAndroid::DeleteSourcePartitions(
    MetadataBuilder* builder,
    uint32_t source_slot,
    const DeltaArchiveManifest& manifest) {
  TEST_AND_RETURN_FALSE(IsRecovery());

  if (IsIncrementalUpdate(manifest)) {
    LOG(ERROR) << "Cannot sideload incremental OTA because snapshots cannot "
               << "be created.";
    if (GetVirtualAbFeatureFlag().IsLaunch()) {
      LOG(ERROR) << "Sideloading incremental updates on devices launches "
                 << " Virtual A/B is not supported.";
    }
    return false;
  }

  LOG(INFO) << "Will overwrite existing partitions. Slot "
            << BootControlInterface::SlotName(source_slot)
            << "may be unbootable until update finishes!";
  const std::string source_suffix = SlotSuffixForSlotNumber(source_slot);
  DeleteGroupsWithSuffix(builder, source_suffix);

  return true;
}

std::unique_ptr<AbstractAction>
DynamicPartitionControlAndroid::GetCleanupPreviousUpdateAction(
    BootControlInterface* boot_control,
    PrefsInterface* prefs,
    CleanupPreviousUpdateActionDelegateInterface* delegate) {
  if (!GetVirtualAbFeatureFlag().IsEnabled()) {
    return std::make_unique<NoOpAction>();
  }
  return std::make_unique<CleanupPreviousUpdateAction>(
      prefs, boot_control, snapshot_.get(), delegate);
}

}  // namespace chromeos_update_engine
