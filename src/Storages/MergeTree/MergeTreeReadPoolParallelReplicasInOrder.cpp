#include <Storages/MergeTree/MergeTreeReadPoolParallelReplicasInOrder.h>

namespace ProfileEvents
{
extern const Event ParallelReplicasReadMarks;
}

namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}

MergeTreeReadPoolParallelReplicasInOrder::MergeTreeReadPoolParallelReplicasInOrder(
    ParallelReadingExtension extension_,
    CoordinationMode mode_,
    RangesInDataParts parts_,
    VirtualFields shared_virtual_fields_,
    const StorageSnapshotPtr & storage_snapshot_,
    const PrewhereInfoPtr & prewhere_info_,
    const ExpressionActionsSettings & actions_settings_,
    const MergeTreeReaderSettings & reader_settings_,
    const Names & column_names_,
    const PoolSettings & settings_,
    const ContextPtr & context_)
    : MergeTreeReadPoolBase(
        std::move(parts_),
        std::move(shared_virtual_fields_),
        storage_snapshot_,
        prewhere_info_,
        actions_settings_,
        reader_settings_,
        column_names_,
        settings_,
        context_)
    , extension(std::move(extension_))
    , mode(mode_)
    , min_marks_per_task(pool_settings.min_marks_for_concurrent_read)
{
    for (const auto & info : per_part_infos)
        min_marks_per_task = std::max(min_marks_per_task, info->min_marks_per_task);

    for (const auto & part : parts_ranges)
        request.push_back({part.data_part->info, MarkRanges{}});

    for (const auto & part : parts_ranges)
        buffered_tasks.push_back({part.data_part->info, MarkRanges{}});

    extension.all_callback(
        InitialAllRangesAnnouncement(mode, parts_ranges.getDescriptions(), extension.number_of_current_replica, /*mark_segment_size_=*/0));
}

MergeTreeReadTaskPtr MergeTreeReadPoolParallelReplicasInOrder::getTask(size_t task_idx, MergeTreeReadTask * previous_task)
{
    std::lock_guard lock(mutex);

    if (task_idx >= per_part_infos.size())
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "Requested task with idx {}, but there are only {} parts",
            task_idx, per_part_infos.size());

    const auto & part_info = per_part_infos[task_idx]->data_part->info;
    auto get_from_buffer = [&]() -> std::optional<MarkRanges>
    {
        for (auto & desc : buffered_tasks)
        {
            if (desc.info == part_info && !desc.ranges.empty())
            {
                auto result = std::move(desc.ranges);
                desc.ranges = MarkRanges{};
                ProfileEvents::increment(ProfileEvents::ParallelReplicasReadMarks, desc.ranges.getNumberOfMarks());
                return result;
            }
        }
        return std::nullopt;
    };

    if (auto result = get_from_buffer())
        return createTask(per_part_infos[task_idx], std::move(*result), previous_task);

    if (no_more_tasks)
        return nullptr;

    auto response
        = extension.callback(ParallelReadRequest(mode, extension.number_of_current_replica, min_marks_per_task * request.size(), request));

    if (!response || response->description.empty() || response->finish)
    {
        no_more_tasks = true;
        return nullptr;
    }

    /// Fill the buffer
    for (size_t i = 0; i < request.size(); ++i)
    {
        auto & new_ranges = response->description[i].ranges;
        auto & old_ranges = buffered_tasks[i].ranges;
        std::move(new_ranges.begin(), new_ranges.end(), std::back_inserter(old_ranges));
    }

    if (auto result = get_from_buffer())
        return createTask(per_part_infos[task_idx], std::move(*result), previous_task);

    return nullptr;
}

}
